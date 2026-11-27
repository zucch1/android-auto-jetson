// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/VideoChannel.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/ipc/VideoSocket.hpp>

#include "VideoReassembler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace aa::ipc {
namespace {

constexpr std::size_t kPathMax = sizeof(sockaddr_un::sun_path);

Error io_error() noexcept { return Error{ErrorCode::transport_io}; }
Error closed_error() noexcept { return Error{ErrorCode::transport_closed}; }

core::Result<void> set_budget(int fd, std::uint32_t budget) noexcept {
    const int value = static_cast<int>(budget);
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) != 0) {
        return io_error();
    }
    int actual = 0;
    socklen_t length = sizeof(actual);
    if (::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual, &length) != 0 ||
        actual > static_cast<int>(budget) * 2) {
        return io_error();
    }
    return {};
}

core::Result<SocketBufferResult> configure_socket_buffer(
    int fd, int option, const protocol::VideoProfile& profile) noexcept {
    const auto requested = recommended_socket_buffer_bytes(profile);
    const int value = static_cast<int>(requested);
    SocketBufferResult result{};
    result.requested_bytes = requested;
    if (::setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value)) != 0) {
        result.setsockopt_errno = errno;
    }
    int actual = 0;
    socklen_t length = sizeof(actual);
    if (::getsockopt(fd, SOL_SOCKET, option, &actual, &length) != 0 || actual < 0) {
        result.getsockopt_errno = (actual < 0) ? EINVAL : errno;
    } else {
        // Record the authoritative raw kernel getsockopt() option value alongside
        // its normalized half on Linux; when setsockopt() fails, getsockopt() still
        // returns the prior option value and short_grant() ignores granted_bytes.
        result.granted_raw_bytes = static_cast<std::uint32_t>(actual);
        result.granted_bytes = result.granted_raw_bytes / 2U;
    }
    return result;
}

core::Result<PeerCredentials> read_peer(int fd) noexcept {
    ucred creds{};
    socklen_t length = sizeof(creds);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creds, &length) != 0) {
        return io_error();
    }
    return PeerCredentials{static_cast<std::uint32_t>(creds.uid),
                           static_cast<std::uint32_t>(creds.pid)};
}

core::Result<void> write_record(int fd, std::span<const std::uint8_t> record,
                                int extra_flags = 0) noexcept {
    ssize_t sent;
    do {
        sent = ::send(fd, record.data(), record.size(), MSG_NOSIGNAL | extra_flags);
    } while (sent < 0 && errno == EINTR);
    if (sent != static_cast<ssize_t>(record.size())) {
        return io_error();
    }
    return {};
}

// Deadline-aware single-descriptor poll shared by the data and accept
// waits. A negative timeout polls with -1 and retries EINTR indefinitely
// without ever constructing a deadline. A nonnegative timeout is clamped
// to the 60 s per-phase bound BEFORE the deadline is built; the remaining
// budget is recomputed before every poll - including every EINTR retry -
// so a signal storm cannot restart a full slice and starve the deadline.
// Zero keeps exactly one poll(..., 0): a deadline check must not turn
// currently-ready data into an unconditional timeout. A positive
// remaining budget polls with a millisecond CEILING (bounded by 60 s) so
// a sub-millisecond remainder neither busy-spins nor times out early
// (<1 ms rounding overrun is acceptable). Status mapping: negative
// non-EINTR -> transport_io, zero -> timeout, positive -> readiness.
core::Result<void> poll_one(int fd, short events, core::Milliseconds timeout) noexcept {
    pollfd ready{fd, events, 0};
    if (timeout.count < 0) {
        for (;;) {
            const int status = ::poll(&ready, 1, -1);
            if (status < 0 && errno == EINTR) {
                continue;
            }
            if (status < 0) {
                return io_error();
            }
            return {};
        }
    }
    const auto bounded = timeout.count > 60000 ? 60000 : timeout.count;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{bounded};
    for (bool initial = true;; initial = false) {
        if (!initial) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                return Error{ErrorCode::timeout};
            }
        }
        int wait = 0;
        if (bounded > 0) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
            std::int64_t millis = (nanos + 999'999) / 1'000'000;
            if (millis < 1) {
                millis = 1;
            }
            if (millis > 60'000) {
                millis = 60'000;
            }
            wait = static_cast<int>(millis);
        }
        const int status = ::poll(&ready, 1, wait);
        if (status < 0 && errno == EINTR) {
            continue;
        }
        if (status < 0) {
            return io_error();
        }
        if (status == 0) {
            return Error{ErrorCode::timeout};
        }
        return {};
    }
}

core::Result<void> await_readable(int fd, core::Milliseconds timeout) noexcept {
    // Readiness only: EOF -> transport_closed is decided AFTER recvmsg() in
    // VideoConsumer::receive(), because queued final records and RDHUP can
    // coexist and a premature closed would discard them.
    return poll_one(fd, static_cast<short>(POLLIN | POLLRDHUP), timeout);
}

core::Result<std::size_t> read_record(int fd, std::span<std::uint8_t> buffer,
                                      core::Milliseconds timeout) noexcept {
    if (auto ready = await_readable(fd, timeout); !ready) {
        return ready.error();
    }
    iovec vector{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    ssize_t size;
    do {
        size = ::recvmsg(fd, &message, 0);
    } while (size < 0 && errno == EINTR);
    if (size < 0) {
        return io_error();
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        return Error{ErrorCode::protocol_malformed_message};
    }
    return static_cast<std::size_t>(size);
}

core::Result<NegotiatedLimits> exchange_limits(int fd, const NegotiatedLimits& local,
                                               core::Milliseconds timeout) noexcept {
    std::array<std::uint8_t, kLimitsRecordBytes> record{};
    encode_limits_record(record, local);
    if (auto sent = write_record(fd, record); !sent) {
        return sent.error();
    }
    std::array<std::uint8_t, kLimitsRecordBytes> peer_record{};
    auto got = read_record(fd, peer_record, timeout);
    if (!got) {
        return got.error();
    }
    if (got.value() != kLimitsRecordBytes) {
        return Error{ErrorCode::protocol_malformed_message};
    }
    auto peer = decode_limits_record(peer_record);
    if (!peer) {
        return peer.error();
    }
    return negotiate_limits(local, peer.value());
}

// Audit L11: the limits handshake is a short exchange of one fixed-size record.
// A peer that connects and then says nothing must not hold the accept loop for
// the caller's whole (up to 60 s, or unbounded) timeout, so every handshake is
// capped at kHandshakeTimeout. The accept loop is single-threaded and the
// listener has a backlog of one, so the worst case for a silent same-UID peer is
// one kHandshakeTimeout of delay per attempt, after which it is closed and the
// next consumer is accepted. A fully non-blocking pending-handshake table would
// remove even that delay but adds per-peer state to an endpoint that admits one
// same-UID consumer at a time; the bounded block is the deliberate trade-off.
constexpr core::Milliseconds kHandshakeTimeout{2000};

core::Milliseconds bounded_handshake_timeout(core::Milliseconds requested) noexcept {
    return (requested.count >= 0 && requested.count < kHandshakeTimeout.count) ? requested
                                                                              : kHandshakeTimeout;
}

// Identity of the endpoint node bound at a path. Captured at bind time so a
// later teardown can prove it still owns the node before unlinking it.
struct EndpointIdentity final {
    dev_t device{};
    ino_t inode{};
};

void unlink_owned_endpoint(const std::string& path, const EndpointIdentity& identity) noexcept {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        return;
    }
    if (info.st_dev != identity.device || info.st_ino != identity.inode) {
        return;
    }
    ::unlink(path.c_str());
}

// Directory-boundary validation shared by StartupLock::acquire and
// prepare_video_socket_path (idempotent, runs on both paths): lstat so a
// symlinked directory is refused rather than followed, a real directory,
// owned by the caller, exact 0700 mode. StartupLock runs this BEFORE the
// lock file is opened or flocked, so an unvalidated directory can never
// host the lock or wedge a starter behind a flock held there.
core::Result<void> validate_runtime_directory(const std::string& directory) noexcept {
    struct stat info {};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return Error{ErrorCode::transport_unavailable};
    }
    if (info.st_uid != ::getuid() || (info.st_mode & 0777) != 0700) {
        return Error{ErrorCode::transport_unavailable};
    }
    return {};
}

// Per-path startup lock for VideoServer::listen: cooperating same-path
// starters serialize on flock(LOCK_EX) over a stable lock inode beside the
// socket (<path>.lock), so the prepare/probe/unlink/bind sequence can never
// interleave between them and exactly one of them binds. The lock file is
// created O_CREAT and never deleted - the stable inode IS the lock identity
// (recreating it would let racers lock different inodes). It is a regular
// file at its own name: the stale-node probe and close() only ever touch the
// socket path, so it confuses neither. flock releases on close even if the
// holder dies, so a crash cannot wedge later starters.
//
// Descriptor lifetime and exclusive-lock lifetime are distinct: listen()
// unlocks the descriptor and transfers it (still open) into the live server,
// so teardown can reacquire LOCK_EX around the close+unlink pair and
// serialize endpoint removal against a cooperating starter. The lock/unlock
// operations never allocate and retry EINTR, so they are usable from
// noexcept cleanup.
class StartupLock final {
public:
    StartupLock() = default;
    StartupLock(const StartupLock&) = delete;
    StartupLock& operator=(const StartupLock&) = delete;
    StartupLock(StartupLock&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    StartupLock& operator=(StartupLock&& other) noexcept {
        if (this != &other) {
            release();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    ~StartupLock() { release(); }

    static core::Result<StartupLock> acquire(const std::string& socket_path) noexcept {
        const auto slash = socket_path.find_last_of('/');
        if (slash == std::string::npos) {
            return Error{ErrorCode::invalid_argument};
        }
        // The lock lives beside the socket, so its directory must exist
        // before prepare_video_socket_path has run; create it exactly as
        // prepare does. The directory boundary is validated BEFORE the lock
        // file is opened or flocked: an unvalidated directory (foreign
        // owner, wrong mode, or a symlink) must fail transport_unavailable
        // without creating a lock in it and without ever waiting on a flock
        // that may be held there.
        const std::string directory = socket_path.substr(0, slash);
        if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
            return Error{ErrorCode::transport_unavailable};
        }
        if (auto validated = validate_runtime_directory(directory); !validated) {
            return validated.error();
        }
        const std::string lock_path = socket_path + ".lock";
        const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) {
            return io_error();
        }
        int status;
        do {
            status = ::flock(fd, LOCK_EX);
        } while (status != 0 && errno == EINTR);
        if (status != 0) {
            ::close(fd);
            return io_error();
        }
        StartupLock lock;
        lock.fd_ = fd;
        return lock;
    }

    [[nodiscard]] core::Result<void> lock_ex() noexcept {
        if (fd_ < 0) {
            return io_error();
        }
        int status;
        do {
            status = ::flock(fd_, LOCK_EX);
        } while (status != 0 && errno == EINTR);
        if (status != 0) {
            return io_error();
        }
        return {};
    }

    [[nodiscard]] core::Result<void> unlock() noexcept {
        if (fd_ < 0) {
            return io_error();
        }
        int status;
        do {
            status = ::flock(fd_, LOCK_UN);
        } while (status != 0 && errno == EINTR);
        if (status != 0) {
            return io_error();
        }
        return {};
    }

    void release() noexcept {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

enum class EndpointState : std::uint8_t { missing, stale, occupied };

// Staleness probe: a datagram connect can never queue a connection on a
// seqpacket or stream listener (the kernel answers EPROTOTYPE), so a live
// endpoint is detected without disturbing it. Refused connect = a dead node.
core::Result<EndpointState> probe_endpoint(const std::string& path) noexcept {
    if (path.size() >= kPathMax) {
        return Error{ErrorCode::invalid_argument};
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return io_error();
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const int failure = errno;
    ::close(fd);
    if (connected == 0) {
        return EndpointState::occupied;
    }
    if (failure == ECONNREFUSED) {
        return EndpointState::stale;
    }
    if (failure == ENOENT) {
        return EndpointState::missing;
    }
    return EndpointState::occupied;
}

int make_seqpacket() noexcept {
    return ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
}

core::Result<EndpointIdentity> bind_path(int fd, const std::string& path) noexcept {
    if (path.size() >= kPathMax) {
        return Error{ErrorCode::invalid_argument};
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    // Audit L11: the endpoint must end up 0600, but umask() is process-global
    // and would hand a concurrently created file (any other thread) the wrong
    // mode. Instead rely on the directory boundary: prepare_video_socket_path
    // already refused anything but a real, caller-owned, exact-0700 directory,
    // so between bind() and the chmod below the new node is unreachable to any
    // other user whatever mode the kernel gave it. The chmod is fail-closed.
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return Error{ErrorCode::transport_unavailable};
    }
    struct stat bound {};
    if (::lstat(path.c_str(), &bound) != 0) {
        return io_error();
    }
    const EndpointIdentity identity{bound.st_dev, bound.st_ino};
    if (::chmod(path.c_str(), 0600) != 0) {
        // Fail closed: never leave an endpoint that was not secured to 0600.
        unlink_owned_endpoint(path, identity);
        return io_error();
    }
    return identity;
}

core::Result<void> connect_path(int fd, const std::string& path,
                                core::Milliseconds timeout) noexcept {
    if (path.size() >= kPathMax) {
        return Error{ErrorCode::invalid_argument};
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return io_error();
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return io_error();
    }
    if (timeout.count < 0) {
        // Documented "blocks": no deadline. Restore blocking mode and connect
        // once, unchanged semantics.
        if (::fcntl(fd, F_SETFL, flags) != 0) {
            return io_error();
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return Error{ErrorCode::transport_unavailable};
        }
        return {};
    }
    // Deadline-bounded connect. The effective bound mirrors the 60 s
    // per-phase clamp in await_readable()/accept_consumer().
    const auto bounded = timeout.count > 60000 ? 60000 : timeout.count;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{bounded};
    const auto attempt = [&]() noexcept -> core::Result<void> {
        int failure = 0;
        for (;;) {
            if (failure != 0) {
                // Every retry - after a backlog refusal, a signal-interrupted
                // connect or a signal-interrupted sleep - passes through this
                // absolute-deadline gate first, so a signal storm cannot keep
                // the loop alive past the bound.
                const auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining <= std::chrono::steady_clock::duration::zero()) {
                    return Error{ErrorCode::timeout};
                }
                if (failure != EINTR) {
                    // Saturated-backlog backoff: sleep at most the remaining
                    // budget (min(10 ms, remaining)); an interrupted sleep
                    // re-enters through the deadline gate instead of
                    // restarting a full slice.
                    const auto slice =
                        std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
                    const int status =
                        ::poll(nullptr, 0, static_cast<int>(slice > 10 ? 10 : slice));
                    if (status < 0 && errno != EINTR) {
                        return io_error();
                    }
                    if (status < 0) {
                        continue;
                    }
                }
            }
            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                return {};
            }
            failure = errno;
            // AF_UNIX SOCK_SEQPACKET connect is synchronous (v6.8 unix_stream_connect):
            // a saturated backlog answers EAGAIN (never EINPROGRESS), completion never
            // writes sk_err (SO_ERROR stays 0), and a fresh fd polls POLLHUP|POLLOUT -
            // so deadline-retry here; never "restore" the TCP poll(POLLOUT)+SO_ERROR idiom.
            if (failure == EISCONN) {
                return {};
            }
            if (failure != EINTR && failure != EAGAIN && failure != EWOULDBLOCK) {
                return Error{ErrorCode::transport_unavailable};
            }
        }
    };
    const auto result = attempt();
    if (::fcntl(fd, F_SETFL, flags) != 0) {
        return io_error();
    }
    return result;
}

} // namespace

VideoSocketPath production_video_socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string base;
    if (runtime != nullptr && runtime[0] != '\0') {
        base = runtime;
    } else {
        base = "/run/user/";
        base += std::to_string(static_cast<unsigned long long>(::getuid()));
    }
    return video_socket_path_in(base);
}

VideoSocketPath video_socket_path_in(const std::string& runtime_dir) {
    std::string value = runtime_dir;
    if (!value.empty() && value.back() != '/') {
        value += '/';
    }
    value += "android-auto-receiver/video.sock";
    return VideoSocketPath{std::move(value)};
}

core::Result<void> prepare_video_socket_path(const VideoSocketPath& path) {
    const auto slash = path.value.find_last_of('/');
    if (slash == std::string::npos) {
        return Error{ErrorCode::invalid_argument};
    }
    const std::string directory = path.value.substr(0, slash);
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        return Error{ErrorCode::transport_unavailable};
    }
    if (auto validated = validate_runtime_directory(directory); !validated) {
        return validated.error();
    }
    struct stat node {};
    if (::lstat(path.value.c_str(), &node) != 0) {
        return errno == ENOENT ? core::Result<void>{}
                               : core::Result<void>{Error{ErrorCode::transport_unavailable}};
    }
    if (!S_ISSOCK(node.st_mode)) {
        return Error{ErrorCode::transport_unavailable};
    }
    const EndpointIdentity identity{node.st_dev, node.st_ino};
    auto state = probe_endpoint(path.value);
    if (!state) {
        return state.error();
    }
    switch (state.value()) {
    case EndpointState::missing:
        return {};
    case EndpointState::stale:
        break;
    case EndpointState::occupied:
        return Error{ErrorCode::transport_unavailable};
    }
    // Identity-checked unlink keeps stale cleanup bound to the probed
    // endpoint and stays inside the startup-lock critical section as defense.
    // Residual: noncooperating same-UID mutation of the lock/socket is out of
    // scope (0700 dir + peer-cred admission); cooperating concurrent starts
    // serialize on the per-path startup lock.
    unlink_owned_endpoint(path.value, identity);
    return {};
}

std::uint32_t current_uid() noexcept { return static_cast<std::uint32_t>(::getuid()); }

constexpr std::uint32_t kStreamStateMagic = 0x41415354U; // "AAST"
constexpr std::uint32_t kStreamStateVersion = 1U;

struct SharedStreamCounters final {
    std::uint32_t magic{kStreamStateMagic};
    std::uint32_t version{kStreamStateVersion};
    std::atomic<std::uint64_t> last_frame{0};
    std::atomic<std::uint64_t> next_sequence{0};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct VideoProducer::Impl final {
    struct PartialSend final {
        bool active{false};
        std::uint64_t frame{};
        std::uint32_t next_index{};
        std::uint32_t count{};
        std::uint64_t first_sequence{};
        std::uint64_t send_started_ns{};
    };

    int fd{-1};
    int state_fd{-1};
    SharedStreamCounters* shared{nullptr};
    NegotiatedLimits limits{};
    std::array<std::uint8_t, kFrameHeaderBytes + kMaxDatagramPayload> packet{};
    std::uint64_t sequence{};
    std::uint64_t last_frame{};
    std::uint64_t dropped{};
    std::uint64_t backpressure{};
    bool waiting_idr{true};
    bool closed{false};
    bool inherited{false};
    PartialSend partial{};
    VideoEndpoint endpoint{};
    VideoEndpointObserver observer{};
    DatagramSend datagram_send{};

    Impl() = default;
    ~Impl() { release(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void record_frame_attempt(std::uint64_t frame) noexcept {
        if (frame > last_frame) {
            last_frame = frame;
        }
        if (shared != nullptr) {
            auto prev = shared->last_frame.load(std::memory_order_relaxed);
            while (frame > prev &&
                   !shared->last_frame.compare_exchange_weak(
                       prev, frame, std::memory_order_release, std::memory_order_relaxed)) {}
        }
    }

    void record_sequence_attempt(std::uint64_t next_seq) noexcept {
        if (shared != nullptr) {
            auto prev = shared->next_sequence.load(std::memory_order_relaxed);
            while (next_seq > prev &&
                   !shared->next_sequence.compare_exchange_weak(
                       prev, next_seq, std::memory_order_release, std::memory_order_relaxed)) {}
        }
    }

    void release() noexcept {
        if (closed) {
            return;
        }
        closed = true;
        partial.active = false;
        if (shared != nullptr) {
            ::munmap(shared, sizeof(SharedStreamCounters));
            shared = nullptr;
        }
        if (state_fd >= 0) {
            ::close(state_fd);
            state_fd = -1;
        }
        if (fd >= 0) {
            if (!inherited) {
                ::shutdown(fd, SHUT_WR);
            }
            ::close(fd);
            fd = -1;
        }
    }
};

VideoProducer::VideoProducer() = default;
VideoProducer::VideoProducer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VideoProducer::VideoProducer(VideoProducer&&) noexcept = default;
VideoProducer& VideoProducer::operator=(VideoProducer&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}
VideoProducer::~VideoProducer() { close(); }

core::Result<VideoProducer> VideoProducer::adopt_accepted(int fd, const NegotiatedLimits& limits,
                                                          int state_fd) {
    int accepting{}, type{};
    sockaddr_un address{};
    socklen_t address_size = sizeof(address);
    socklen_t size = sizeof(type);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
        address.sun_family != AF_UNIX ||
        ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_SEQPACKET ||
        ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size) != 0 || accepting ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return io_error();
    }
    auto peer = read_peer(fd);
    if (!peer) {
        return peer.error();
    }
    SharedStreamCounters* counters = nullptr;
    if (state_fd >= 0) {
        struct stat info {};
        if (::fstat(state_fd, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < static_cast<off_t>(sizeof(SharedStreamCounters)) ||
            ::fcntl(state_fd, F_SETFD, FD_CLOEXEC) != 0) {
            return io_error();
        }
        void* mapped = ::mmap(nullptr, sizeof(SharedStreamCounters), PROT_READ | PROT_WRITE,
                              MAP_SHARED, state_fd, 0);
        if (mapped == MAP_FAILED) {
            return io_error();
        }
        counters = static_cast<SharedStreamCounters*>(mapped);
        if (counters->magic != kStreamStateMagic || counters->version != kStreamStateVersion) {
            ::munmap(mapped, sizeof(SharedStreamCounters));
            return io_error();
        }
    }
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;
    impl->state_fd = state_fd;
    impl->shared = counters;
    impl->limits = limits;
    impl->inherited = true;
    if (counters != nullptr) {
        impl->last_frame = counters->last_frame.load(std::memory_order_acquire);
        impl->sequence = counters->next_sequence.load(std::memory_order_acquire);
    }
    impl->endpoint = VideoEndpoint{VideoEndpointKind::send_started,
        static_cast<std::uint32_t>(::getpid()), peer.value().pid, video_monotonic_ns()};
    return VideoProducer{std::move(impl)};
}

core::Result<int> VideoProducer::prepare_shared_state() {
    if (!impl_ || impl_->closed || impl_->fd < 0) {
        return closed_error();
    }
    if (impl_->state_fd >= 0 && impl_->shared != nullptr) {
        return impl_->state_fd;
    }
    const int mfd = ::memfd_create("aa-video-stream", MFD_CLOEXEC);
    if (mfd < 0) {
        return io_error();
    }
    if (::ftruncate(mfd, static_cast<off_t>(sizeof(SharedStreamCounters))) != 0) {
        ::close(mfd);
        return io_error();
    }
    void* mapped = ::mmap(nullptr, sizeof(SharedStreamCounters), PROT_READ | PROT_WRITE,
                          MAP_SHARED, mfd, 0);
    if (mapped == MAP_FAILED) {
        ::close(mfd);
        return io_error();
    }
    auto* counters = new (mapped) SharedStreamCounters{};
    counters->last_frame.store(impl_->last_frame, std::memory_order_relaxed);
    counters->next_sequence.store(impl_->sequence, std::memory_order_release);
    impl_->state_fd = mfd;
    impl_->shared = counters;
    return mfd;
}

core::Result<VideoProducer> VideoProducer::borrow() {
    auto state = prepare_shared_state();
    if (!state) {
        return state.error();
    }
    const int dup_fd = ::fcntl(impl_->fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        return io_error();
    }
    const int dup_state = ::fcntl(state.value(), F_DUPFD_CLOEXEC, 0);
    if (dup_state < 0) {
        ::close(dup_fd);
        return io_error();
    }
    auto adopted = adopt_accepted(dup_fd, impl_->limits, dup_state);
    if (!adopted) {
        ::close(dup_fd);
        ::close(dup_state);
        return adopted.error();
    }
    return adopted;
}

core::Result<SendStatus> VideoProducer::send_access_unit(const VideoSendRequest& request) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    if (request.profile && !protocol::valid_video_profile(*request.profile)) {
        return Error{ErrorCode::channel_caps_unsupported};
    }
    if (request.payload.empty()) {
        return Error{ErrorCode::invalid_argument};
    }
    const auto cap = static_cast<std::size_t>(impl_->limits.max_access_unit_bytes);
    if (request.payload.size() > cap) {
        return Error{ErrorCode::transport_oversize_frame};
    }
    impl_->partial.active = false;
    if (impl_->waiting_idr && !request.idr) {
        ++impl_->dropped;
        return SendStatus::dropped_waiting_idr;
    }
    const std::size_t datagram = impl_->limits.max_datagram_payload;
    const auto count =
        static_cast<std::uint32_t>((request.payload.size() + datagram - 1) / datagram);
    const auto first_sequence = impl_->sequence;
    std::uint64_t send_started_ns{};
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto length = std::min<std::size_t>(
            datagram, request.payload.size() - static_cast<std::size_t>(index) * datagram);
        const auto seq = impl_->sequence++;
        if (index == 0) {
            impl_->record_frame_attempt(request.frame);
        }
        impl_->record_sequence_attempt(seq + 1);
        VideoFrameHeader header{request.frame, seq, request.timestamp_ns,
                                index,          count,
                                static_cast<std::uint32_t>(request.payload.size()),
                                static_cast<std::uint32_t>(length), request.idr, request.profile};
        encode_frame_header(std::span<std::uint8_t>{impl_->packet.data(), kFrameHeaderBytes},
                            header);
        std::memcpy(impl_->packet.data() + kFrameHeaderBytes,
                    request.payload.data() + static_cast<std::size_t>(index) * datagram, length);
        std::ptrdiff_t sent;
        int send_errno = 0;
        if (index == 0) {
            send_started_ns = video_monotonic_ns();
        }
        do {
            if (impl_->datagram_send) {
                sent = impl_->datagram_send(impl_->fd, impl_->packet.data(),
                                            kFrameHeaderBytes + length,
                                            MSG_DONTWAIT | MSG_NOSIGNAL, send_errno);
            } else {
                sent = ::send(impl_->fd, impl_->packet.data(), kFrameHeaderBytes + length,
                              MSG_DONTWAIT | MSG_NOSIGNAL);
                send_errno = (sent < 0) ? errno : 0;
            }
        } while (sent < 0 && send_errno == EINTR);
        if (sent != static_cast<ssize_t>(kFrameHeaderBytes + length)) {
            impl_->waiting_idr = true;
            ++impl_->dropped;
            if (sent < 0 && (send_errno == EAGAIN || send_errno == EWOULDBLOCK)) {
                ++impl_->backpressure;
                return SendStatus::dropped_backpressure;
            }
            return io_error();
        }
    }
    impl_->waiting_idr = false;
    if (impl_->observer) {
        auto event = impl_->endpoint;
        event.frame = request.frame;
        event.sequence = first_sequence;
        event.timestamp_ns = request.timestamp_ns;
        event.observed_ns = send_started_ns;
        event.payload_bytes = request.payload.size();
        impl_->observer(event);
    }
    return SendStatus::sent;
}

core::Result<TrySendStatus> VideoProducer::try_send_access_unit(const VideoSendRequest& request) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    if (request.profile && !protocol::valid_video_profile(*request.profile)) {
        return Error{ErrorCode::channel_caps_unsupported};
    }
    if (request.payload.empty()) {
        return Error{ErrorCode::invalid_argument};
    }
    const auto cap = static_cast<std::size_t>(impl_->limits.max_access_unit_bytes);
    if (request.payload.size() > cap) {
        return Error{ErrorCode::transport_oversize_frame};
    }
    if (impl_->waiting_idr && !request.idr) {
        impl_->partial.active = false;
        ++impl_->dropped;
        return TrySendStatus::dropped_waiting_idr;
    }
    const std::size_t datagram = impl_->limits.max_datagram_payload;
    const auto count =
        static_cast<std::uint32_t>((request.payload.size() + datagram - 1) / datagram);

    std::uint32_t start_index = 0;
    std::uint64_t first_sequence = impl_->sequence;
    std::uint64_t send_started_ns{};
    if (impl_->partial.active) {
        if (impl_->partial.frame != request.frame || impl_->partial.count != count) {
            impl_->partial.active = false;
            impl_->waiting_idr = true;
            ++impl_->dropped;
            if (!request.idr) {
                return TrySendStatus::dropped_waiting_idr;
            }
        } else {
            start_index = impl_->partial.next_index;
            first_sequence = impl_->partial.first_sequence;
            send_started_ns = impl_->partial.send_started_ns;
        }
    }

    for (std::uint32_t index = start_index; index < count; ++index) {
        const auto length = std::min<std::size_t>(
            datagram, request.payload.size() - static_cast<std::size_t>(index) * datagram);
        if (index == 0) {
            impl_->record_frame_attempt(request.frame);
        }
        impl_->record_sequence_attempt(impl_->sequence + 1);
        VideoFrameHeader header{request.frame, impl_->sequence, request.timestamp_ns,
                                index,          count,
                                static_cast<std::uint32_t>(request.payload.size()),
                                static_cast<std::uint32_t>(length), request.idr, request.profile};
        encode_frame_header(std::span<std::uint8_t>{impl_->packet.data(), kFrameHeaderBytes},
                            header);
        std::memcpy(impl_->packet.data() + kFrameHeaderBytes,
                    request.payload.data() + static_cast<std::size_t>(index) * datagram, length);
        if (index == 0) {
            send_started_ns = video_monotonic_ns();
        }
        std::ptrdiff_t sent;
        int send_errno = 0;
        do {
            if (impl_->datagram_send) {
                sent = impl_->datagram_send(impl_->fd, impl_->packet.data(),
                                            kFrameHeaderBytes + length,
                                            MSG_DONTWAIT | MSG_NOSIGNAL, send_errno);
            } else {
                sent = ::send(impl_->fd, impl_->packet.data(), kFrameHeaderBytes + length,
                              MSG_DONTWAIT | MSG_NOSIGNAL);
                send_errno = (sent < 0) ? errno : 0;
            }
        } while (sent < 0 && send_errno == EINTR);
        if (sent == static_cast<ssize_t>(kFrameHeaderBytes + length)) {
            ++impl_->sequence;
            continue;
        }
        if (sent < 0 && (send_errno == EAGAIN || send_errno == EWOULDBLOCK)) {
            if (index > 0) {
                impl_->partial = {true, request.frame, index, count, first_sequence, send_started_ns};
            }
            return TrySendStatus::would_block;
        }
        impl_->partial.active = false;
        impl_->waiting_idr = true;
        ++impl_->dropped;
        return io_error();
    }
    impl_->partial.active = false;
    impl_->waiting_idr = false;
    if (impl_->observer) {
        auto event = impl_->endpoint;
        event.frame = request.frame;
        event.sequence = first_sequence;
        event.timestamp_ns = request.timestamp_ns;
        event.observed_ns = send_started_ns;
        event.payload_bytes = request.payload.size();
        impl_->observer(event);
    }
    return TrySendStatus::sent;
}

void VideoProducer::enter_waiting_idr_on_backpressure() noexcept {
    if (!impl_) {
        return;
    }
    impl_->partial.active = false;
    impl_->waiting_idr = true;
    ++impl_->dropped;
    ++impl_->backpressure;
}

void VideoProducer::abandon_partial_send() noexcept {
    if (!impl_) {
        return;
    }
    impl_->partial.active = false;
    impl_->waiting_idr = true;
}

void VideoProducer::set_datagram_send_for_test(DatagramSend send_fn) {
    if (impl_) {
        impl_->datagram_send = std::move(send_fn);
    }
}

void VideoProducer::set_observer(VideoEndpointObserver observer) {
    impl_->observer = std::move(observer);
}

void VideoProducer::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

core::Result<SocketBufferResult> VideoProducer::configure_send_buffer(
    const protocol::VideoProfile& profile) noexcept {
    if (!impl_ || impl_->closed || impl_->fd < 0) {
        return closed_error();
    }
    return configure_socket_buffer(impl_->fd, SO_SNDBUF, profile);
}

bool VideoProducer::closed() const noexcept { return !impl_ || impl_->closed; }
bool VideoProducer::peer_closed() const noexcept {
    if (!impl_ || impl_->closed || impl_->fd < 0) {
        return true;
    }
    pollfd ready{impl_->fd, static_cast<short>(POLLRDHUP), 0};
    int status;
    do {
        status = ::poll(&ready, 1, 0);
    } while (status < 0 && errno == EINTR);
    if (status < 0) {
        return true;
    }
    return status > 0 && (ready.revents & (POLLHUP | POLLRDHUP | POLLERR | POLLNVAL)) != 0;
}
bool VideoProducer::waiting_idr() const noexcept { return !impl_ || impl_->waiting_idr; }
const NegotiatedLimits& VideoProducer::limits() const noexcept { return impl_->limits; }
std::uint64_t VideoProducer::last_frame_id() const noexcept {
    if (!impl_) {
        return 0;
    }
    if (impl_->shared != nullptr) {
        return impl_->shared->last_frame.load(std::memory_order_acquire);
    }
    return impl_->last_frame;
}
std::uint64_t VideoProducer::next_sequence() const noexcept {
    if (!impl_) {
        return 0;
    }
    if (impl_->shared != nullptr) {
        return impl_->shared->next_sequence.load(std::memory_order_acquire);
    }
    return impl_->sequence;
}
std::uint64_t VideoProducer::dropped() const noexcept { return impl_->dropped; }
std::uint64_t VideoProducer::backpressure_events() const noexcept { return impl_->backpressure; }
int VideoProducer::native_handle() const noexcept { return impl_->fd; }

struct VideoConsumer::Impl final {
    int fd{-1};
    NegotiatedLimits limits{};
    std::unique_ptr<VideoReassembler> assembler;
    std::array<std::uint8_t, kFrameHeaderBytes + kMaxDatagramPayload> packet{};
    VideoStreamCapture* capture{nullptr};
    bool closed{false};
    bool eof{false};
    VideoEndpoint endpoint{};
    VideoEndpointObserver observer{};

    Impl() = default;
    ~Impl() { release(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void release() noexcept {
        if (closed) {
            return;
        }
        closed = true;
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            fd = -1;
        }
    }
};

VideoConsumer::VideoConsumer() = default;
VideoConsumer::VideoConsumer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VideoConsumer::VideoConsumer(VideoConsumer&&) noexcept = default;
VideoConsumer& VideoConsumer::operator=(VideoConsumer&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}
VideoConsumer::~VideoConsumer() { close(); }

core::Result<ReceiveStatus> VideoConsumer::receive(core::Milliseconds timeout) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    if (impl_->eof) {
        return closed_error();
    }
    std::array<std::uint8_t, kFrameHeaderBytes + kMaxDatagramPayload> scratch{};
    iovec vector{scratch.data(), scratch.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    auto ready = await_readable(impl_->fd, timeout);
    if (!ready) {
        return ready.error();
    }
    ssize_t size;
    do {
        size = ::recvmsg(impl_->fd, &message, 0);
    } while (size < 0 && errno == EINTR);
    if (size < 0) {
        return io_error();
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        impl_->assembler->reject();
        return Error{ErrorCode::transport_oversize_frame};
    }
    const auto received = static_cast<std::size_t>(size);
    if (received == 0) {
        pollfd ready{impl_->fd, POLLRDHUP, 0};
        int status;
        do {
            status = ::poll(&ready, 1, 0);
        } while (status < 0 && errno == EINTR);
        if (status >= 0 && (ready.revents & (POLLRDHUP | POLLHUP))) {
            impl_->eof = true;
            return closed_error();
        }
        impl_->assembler->reject();
        return Error{ErrorCode::transport_malformed_frame};
    }
    const auto header =
        decode_frame_header(std::span<const std::uint8_t>{scratch.data(), received}, impl_->limits);
    if (!header) {
        impl_->assembler->reject();
        return header.error();
    }
    const auto step = impl_->assembler->accept(
        header.value(),
        std::span<const std::uint8_t>{scratch.data() + kFrameHeaderBytes,
                                      header.value().payload_bytes});
    if (!step) {
        return step.error();
    }
    if (step.value() == ReceiveStatus::complete && impl_->observer) {
        const auto& unit = impl_->assembler->unit();
        auto event = impl_->endpoint;
        event.observed_ns = video_monotonic_ns();
        event.frame = unit.frame;
        event.sequence = unit.sequence;
        event.timestamp_ns = unit.timestamp_ns;
        event.payload_bytes = unit.payload.size();
        impl_->observer(event);
    }
    if (step.value() == ReceiveStatus::complete && impl_->capture != nullptr) {
        if (auto recorded = impl_->capture->on_access_unit(impl_->assembler->unit()); !recorded) {
            return recorded.error();
        }
    }
    return step.value();
}

const VideoAccessUnit& VideoConsumer::access_unit() const noexcept {
    return impl_->assembler->unit();
}

void VideoConsumer::set_capture(VideoStreamCapture* capture) noexcept {
    impl_->capture = capture;
}

void VideoConsumer::set_observer(VideoEndpointObserver observer) {
    impl_->observer = std::move(observer);
}

void VideoConsumer::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

core::Result<SocketBufferResult> VideoConsumer::configure_receive_buffer(
    const protocol::VideoProfile& profile) noexcept {
    if (!impl_ || impl_->closed || impl_->fd < 0) {
        return closed_error();
    }
    return configure_socket_buffer(impl_->fd, SO_RCVBUF, profile);
}

bool VideoConsumer::closed() const noexcept { return !impl_ || impl_->closed; }
bool VideoConsumer::waiting_idr() const noexcept { return impl_->assembler->waiting_idr(); }
const NegotiatedLimits& VideoConsumer::limits() const noexcept { return impl_->limits; }
std::uint64_t VideoConsumer::resyncs() const noexcept { return impl_->assembler->resyncs(); }
std::uint64_t VideoConsumer::rejected() const noexcept { return impl_->assembler->rejected(); }
std::uint64_t VideoConsumer::suppressed() const noexcept { return impl_->assembler->suppressed(); }
int VideoConsumer::native_handle() const noexcept { return impl_->fd; }

struct VideoServer::Impl final {
    struct PendingHandshake final {
        int fd{-1};
        PeerCredentials peer{};
        std::chrono::steady_clock::time_point deadline{};

        void close() noexcept {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
            peer = {};
            deadline = {};
        }
    };

    int fd{-1};
    VideoSocketPath path{};
    EndpointIdentity identity{};
    VideoServerOptions options{};
    StartupLock lock{};
    PendingHandshake pending{};
    std::uint64_t unauthorized{};
    bool closed{false};
    bool inherited{false};

    Impl() = default;
    ~Impl() { release(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void release() noexcept {
        if (closed) {
            return;
        }
        closed = true;
        pending.close();
        if (fd < 0) {
            // Uninitialized: listen() failed before adopting the listener,
            // so there is no endpoint to clean up and no lock to reacquire.
            return;
        }
        if (inherited) {
            ::close(fd);
            fd = -1;
            return;
        }
        // Reacquire the startup lock while the listener is still live so a
        // cooperating starter can neither classify the endpoint stale nor
        // replace it inside the close+unlink window (the unlocked
        // lstat/unlink pair let a previously-validated unlink delete a
        // concurrently bound replacement). If reacquisition fails, close the
        // descriptors but skip the unlink: a stale node is recoverable by
        // the next startup, an unlocked unlink is not.
        const bool held = static_cast<bool>(lock.lock_ex());
        ::close(fd);
        fd = -1;
        if (held) {
            unlink_owned_endpoint(path.value, identity);
        }
        lock.release();
    }
};

VideoServer::VideoServer() = default;
VideoServer::VideoServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
VideoServer::VideoServer(VideoServer&&) noexcept = default;
VideoServer& VideoServer::operator=(VideoServer&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

VideoServer::~VideoServer() { close(); }

core::Result<VideoServer> VideoServer::listen(const VideoSocketPath& path,
                                              const VideoServerOptions& options) {
    // The startup lock is held across the whole prepare/bind/listen section
    // and released when this function returns: cooperating same-path starters
    // serialize here, so the kernel bind race is impossible among them and
    // exactly one of them wins. The descriptor is retained (unlocked) in the
    // returned server so teardown can reacquire it.
    auto startup = StartupLock::acquire(path.value);
    if (!startup) {
        return startup.error();
    }
    if (auto prepared = prepare_video_socket_path(path); !prepared) {
        return prepared.error();
    }
    // Allocate the Impl and copy path/options BEFORE the socket exists: a
    // failure below then destroys only an uninitialized Impl (fd == -1),
    // which must not attempt endpoint cleanup or lock acquisition while the
    // local critical section still owns both.
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->options = options;
    const int fd = make_seqpacket();
    if (fd < 0) {
        return io_error();
    }
    auto bound = bind_path(fd, path.value);
    if (!bound) {
        ::close(fd);
        return bound.error();
    }
    if (::listen(fd, 1) != 0) {
        ::close(fd);
        unlink_owned_endpoint(path.value, bound.value());
        return io_error();
    }
    impl->identity = bound.value();
    if (auto unlocked = startup.value().unlock(); !unlocked) {
        // Never return a live server with an accidentally retained exclusive
        // lock: clean up the fresh listener/endpoint while the local critical
        // section still owns cleanup.
        ::close(fd);
        unlink_owned_endpoint(path.value, bound.value());
        return unlocked.error();
    }
    impl->lock = std::move(startup.value());
    impl->fd = fd;
    return VideoServer{std::move(impl)};
}

core::Result<VideoServer> VideoServer::adopt_listener(int fd, const VideoServerOptions& options) {
    int accepting{}, type{};
    sockaddr_un address{};
    socklen_t address_size = sizeof(address);
    socklen_t size = sizeof(type);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
        address.sun_family != AF_UNIX ||
        ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_SEQPACKET ||
        ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size) != 0 || !accepting ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return io_error();
    }
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;
    impl->options = options;
    impl->inherited = true;
    return VideoServer{std::move(impl)};
}

core::Result<VideoProducer> VideoServer::accept_consumer(core::Milliseconds timeout) {
    return accept_consumer(timeout, kHandshakeTimeout);
}

core::Result<VideoProducer> VideoServer::accept_consumer(core::Milliseconds accept_timeout,
                                                         core::Milliseconds handshake_timeout) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    const auto call_start = std::chrono::steady_clock::now();
    const bool has_call_deadline = accept_timeout.count > 0;
    const auto call_deadline =
        has_call_deadline
            ? call_start + std::chrono::milliseconds{
                               accept_timeout.count > 60000 ? 60000 : accept_timeout.count}
            : std::chrono::steady_clock::time_point{};

    const auto remaining_call_wait = [&]() noexcept -> core::Milliseconds {
        if (accept_timeout.count < 0) {
            return core::Milliseconds{-1};
        }
        if (accept_timeout.count == 0) {
            return core::Milliseconds{0};
        }
        const auto remaining = call_deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            return core::Milliseconds{0};
        }
        const auto nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
        return core::Milliseconds{std::max<std::int64_t>(1, (nanos + 999'999) / 1'000'000)};
    };

    const auto complete_pending = [&](core::Milliseconds wait) -> core::Result<VideoProducer> {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining_hs = impl_->pending.deadline - now;
        core::Milliseconds poll_wait{0};
        if (remaining_hs > std::chrono::steady_clock::duration::zero()) {
            const auto hs_nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(remaining_hs).count();
            const std::int64_t hs_ms = std::max<std::int64_t>(1, (hs_nanos + 999'999) / 1'000'000);
            if (wait.count < 0) {
                poll_wait = core::Milliseconds{hs_ms};
            } else {
                poll_wait = core::Milliseconds{std::min(wait.count, hs_ms)};
            }
        }
        auto ready = await_readable(impl_->pending.fd, poll_wait);
        if (!ready) {
            if (ready.error().code() == ErrorCode::timeout) {
                if (std::chrono::steady_clock::now() >= impl_->pending.deadline) {
                    impl_->pending.close();
                }
                return Error{ErrorCode::timeout};
            }
            impl_->pending.close();
            return ready.error();
        }
        std::array<std::uint8_t, kLimitsRecordBytes> peer_record{};
        auto got = read_record(impl_->pending.fd, peer_record, core::Milliseconds{0});
        if (!got) {
            impl_->pending.close();
            return got.error();
        }
        if (got.value() != kLimitsRecordBytes) {
            impl_->pending.close();
            return Error{ErrorCode::protocol_malformed_message};
        }
        auto peer_limits = decode_limits_record(peer_record);
        if (!peer_limits) {
            impl_->pending.close();
            return peer_limits.error();
        }
        auto negotiated = negotiate_limits(impl_->options.local_limits, peer_limits.value());
        if (!negotiated) {
            impl_->pending.close();
            return negotiated.error();
        }
        if (auto budget = set_budget(impl_->pending.fd, negotiated.value().socket_budget_bytes);
            !budget) {
            impl_->pending.close();
            return budget.error();
        }
        auto producer_impl = std::make_unique<VideoProducer::Impl>();
        producer_impl->fd = impl_->pending.fd;
        producer_impl->limits = negotiated.value();
        producer_impl->endpoint = VideoEndpoint{VideoEndpointKind::send_started,
            static_cast<std::uint32_t>(::getpid()), impl_->pending.peer.pid, video_monotonic_ns()};
        impl_->pending.fd = -1;
        impl_->pending.peer = {};
        impl_->pending.deadline = {};
        return VideoProducer{std::move(producer_impl)};
    };

    if (impl_->pending.fd >= 0) {
        return complete_pending(accept_timeout);
    }

    if (auto ready = poll_one(impl_->fd, static_cast<short>(POLLIN), accept_timeout); !ready) {
        return ready.error();
    }
    int fd;
    do {
        fd = ::accept4(impl_->fd, nullptr, nullptr, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return io_error();
    }
    auto peer = read_peer(fd);
    if (!peer) {
        ::close(fd);
        return peer.error();
    }
    if (auto authorized = authorize_peer(peer.value(), impl_->options.expected_peer_uid);
        !authorized) {
        ++impl_->unauthorized;
        ::close(fd);
        return authorized.error();
    }
    std::array<std::uint8_t, kLimitsRecordBytes> record{};
    encode_limits_record(record, impl_->options.local_limits);
    if (auto sent = write_record(fd, record, MSG_DONTWAIT); !sent) {
        ::close(fd);
        return sent.error();
    }
    const auto hs_budget = bounded_handshake_timeout(handshake_timeout);
    impl_->pending.fd = fd;
    impl_->pending.peer = peer.value();
    impl_->pending.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{hs_budget.count};
    return complete_pending(remaining_call_wait());
}

bool VideoServer::has_pending_handshake() const noexcept {
    return impl_ && !impl_->closed && impl_->pending.fd >= 0;
}

void VideoServer::cancel_pending_handshake() noexcept {
    if (impl_) {
        impl_->pending.close();
    }
}

void VideoServer::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

bool VideoServer::closed() const noexcept { return !impl_ || impl_->closed; }
const VideoSocketPath& VideoServer::path() const noexcept { return impl_->path; }
std::uint64_t VideoServer::unauthorized_peers() const noexcept { return impl_->unauthorized; }
int VideoServer::native_handle() const noexcept { return impl_->fd; }

core::Result<VideoConsumer> connect_video_consumer(const VideoSocketPath& path,
                                                   const VideoClientOptions& options) {
    const auto connection_ns = video_monotonic_ns();
    const int fd = make_seqpacket();
    if (fd < 0) {
        return io_error();
    }
    if (auto connected = connect_path(fd, path.value, options.timeout); !connected) {
        ::close(fd);
        return connected.error();
    }
    auto peer = read_peer(fd);
    if (!peer) {
        ::close(fd);
        return peer.error();
    }
    if (auto authorized = authorize_peer(peer.value(), options.expected_server_uid); !authorized) {
        ::close(fd);
        return authorized.error();
    }
    auto negotiated =
        exchange_limits(fd, options.local_limits, bounded_handshake_timeout(options.timeout));
    if (!negotiated) {
        ::close(fd);
        return negotiated.error();
    }
    if (auto budget = set_budget(fd, negotiated.value().socket_budget_bytes); !budget) {
        ::close(fd);
        return budget.error();
    }
    auto consumer_impl = std::make_unique<VideoConsumer::Impl>();
    consumer_impl->fd = fd;
    consumer_impl->limits = negotiated.value();
    consumer_impl->endpoint = VideoEndpoint{VideoEndpointKind::receive_completed,
        peer.value().pid, static_cast<std::uint32_t>(::getpid()), connection_ns};
    consumer_impl->assembler =
        std::make_unique<VideoReassembler>(consumer_impl->limits);
    return VideoConsumer{std::move(consumer_impl)};
}

} // namespace aa::ipc
