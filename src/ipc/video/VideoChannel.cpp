// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/VideoChannel.hpp>
#include <aa/ipc/VideoSocket.hpp>

#include "VideoReassembler.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>
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

core::Result<PeerCredentials> read_peer(int fd) noexcept {
    ucred creds{};
    socklen_t length = sizeof(creds);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creds, &length) != 0) {
        return io_error();
    }
    return PeerCredentials{static_cast<std::uint32_t>(creds.uid),
                           static_cast<std::uint32_t>(creds.pid)};
}

core::Result<void> write_record(int fd, std::span<const std::uint8_t> record) noexcept {
    ssize_t sent;
    do {
        sent = ::send(fd, record.data(), record.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent != static_cast<ssize_t>(record.size())) {
        return io_error();
    }
    return {};
}

core::Result<void> await_readable(int fd, core::Milliseconds timeout) noexcept {
    pollfd ready{fd, POLLIN | POLLRDHUP, 0};
    int status;
    const int millis = timeout.count < 0
                           ? -1
                           : static_cast<int>(timeout.count > 60000 ? 60000 : timeout.count);
    do {
        status = ::poll(&ready, 1, millis);
    } while (status < 0 && errno == EINTR);
    if (status < 0) {
        return io_error();
    }
    if (status == 0) {
        return Error{ErrorCode::timeout};
    }
    return {};
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
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        (void)timeout;
        return Error{ErrorCode::transport_unavailable};
    }
    return {};
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
    struct stat info {};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return Error{ErrorCode::transport_unavailable};
    }
    if (info.st_uid != ::getuid() || (info.st_mode & 0777) != 0700) {
        return Error{ErrorCode::transport_unavailable};
    }
    struct stat node {};
    if (::lstat(path.value.c_str(), &node) != 0) {
        return errno == ENOENT ? core::Result<void>{}
                               : core::Result<void>{Error{ErrorCode::transport_unavailable}};
    }
    if (!S_ISSOCK(node.st_mode)) {
        return Error{ErrorCode::transport_unavailable};
    }
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
    if (::unlink(path.value.c_str()) != 0 && errno != ENOENT) {
        return Error{ErrorCode::transport_unavailable};
    }
    return {};
}

std::uint32_t current_uid() noexcept { return static_cast<std::uint32_t>(::getuid()); }

struct VideoProducer::Impl final {
    int fd{-1};
    NegotiatedLimits limits{};
    std::array<std::uint8_t, kFrameHeaderBytes + kMaxDatagramPayload> packet{};
    std::uint64_t sequence{};
    std::uint64_t dropped{};
    std::uint64_t backpressure{};
    bool waiting_idr{true};
    bool closed{false};

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
            ::shutdown(fd, SHUT_WR);
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

core::Result<SendStatus> VideoProducer::send_access_unit(const VideoSendRequest& request) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    if (request.payload.empty()) {
        return Error{ErrorCode::invalid_argument};
    }
    const auto cap = static_cast<std::size_t>(impl_->limits.max_access_unit_bytes);
    if (request.payload.size() > cap) {
        return Error{ErrorCode::transport_oversize_frame};
    }
    if (impl_->waiting_idr && !request.idr) {
        ++impl_->dropped;
        return SendStatus::dropped_waiting_idr;
    }
    const std::size_t datagram = impl_->limits.max_datagram_payload;
    const auto count =
        static_cast<std::uint32_t>((request.payload.size() + datagram - 1) / datagram);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto length = std::min<std::size_t>(
            datagram, request.payload.size() - static_cast<std::size_t>(index) * datagram);
        VideoFrameHeader header{request.frame, impl_->sequence++, request.timestamp_ns,
                                index,          count,
                                static_cast<std::uint32_t>(request.payload.size()),
                                static_cast<std::uint32_t>(length), request.idr};
        encode_frame_header(std::span<std::uint8_t>{impl_->packet.data(), kFrameHeaderBytes},
                            header);
        std::memcpy(impl_->packet.data() + kFrameHeaderBytes,
                    request.payload.data() + static_cast<std::size_t>(index) * datagram, length);
        ssize_t sent;
        do {
            sent = ::send(impl_->fd, impl_->packet.data(), kFrameHeaderBytes + length,
                          MSG_DONTWAIT | MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);
        if (sent != static_cast<ssize_t>(kFrameHeaderBytes + length)) {
            impl_->waiting_idr = true;
            ++impl_->dropped;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ++impl_->backpressure;
                return SendStatus::dropped_backpressure;
            }
            return io_error();
        }
    }
    impl_->waiting_idr = false;
    return SendStatus::sent;
}

void VideoProducer::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

bool VideoProducer::closed() const noexcept { return !impl_ || impl_->closed; }
const NegotiatedLimits& VideoProducer::limits() const noexcept { return impl_->limits; }
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
        impl_->assembler->loss();
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
        impl_->assembler->loss();
        return Error{ErrorCode::transport_malformed_frame};
    }
    const auto header =
        decode_frame_header(std::span<const std::uint8_t>{scratch.data(), received}, impl_->limits);
    if (!header) {
        impl_->assembler->loss();
        return header.error();
    }
    const auto step = impl_->assembler->accept(
        header.value(),
        std::span<const std::uint8_t>{scratch.data() + kFrameHeaderBytes,
                                      header.value().payload_bytes});
    if (!step) {
        return step.error();
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

void VideoConsumer::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

bool VideoConsumer::closed() const noexcept { return !impl_ || impl_->closed; }
bool VideoConsumer::waiting_idr() const noexcept { return impl_->assembler->waiting_idr(); }
const NegotiatedLimits& VideoConsumer::limits() const noexcept { return impl_->limits; }
std::uint64_t VideoConsumer::resyncs() const noexcept { return impl_->assembler->resyncs(); }
std::uint64_t VideoConsumer::rejected() const noexcept { return impl_->assembler->rejected(); }
std::uint64_t VideoConsumer::suppressed() const noexcept { return impl_->assembler->suppressed(); }
int VideoConsumer::native_handle() const noexcept { return impl_->fd; }

struct VideoServer::Impl final {
    int fd{-1};
    VideoSocketPath path{};
    EndpointIdentity identity{};
    VideoServerOptions options{};
    std::uint64_t unauthorized{};
    bool closed{false};

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
            ::close(fd);
            fd = -1;
        }
        unlink_owned_endpoint(path.value, identity);
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
    if (auto prepared = prepare_video_socket_path(path); !prepared) {
        return prepared.error();
    }
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
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;
    impl->path = path;
    impl->identity = bound.value();
    impl->options = options;
    return VideoServer{std::move(impl)};
}

core::Result<VideoProducer> VideoServer::accept_consumer(core::Milliseconds timeout) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    pollfd ready{impl_->fd, POLLIN, 0};
    const int millis = timeout.count < 0 ? -1
                                         : static_cast<int>(timeout.count > 60000 ? 60000
                                                                                  : timeout.count);
    int status;
    do {
        status = ::poll(&ready, 1, millis);
    } while (status < 0 && errno == EINTR);
    if (status < 0) {
        return io_error();
    }
    if (status == 0) {
        return Error{ErrorCode::timeout};
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
    auto negotiated = exchange_limits(fd, impl_->options.local_limits, timeout);
    if (!negotiated) {
        ::close(fd);
        return negotiated.error();
    }
    if (auto budget = set_budget(fd, negotiated.value().socket_budget_bytes); !budget) {
        ::close(fd);
        return budget.error();
    }
    auto producer_impl = std::make_unique<VideoProducer::Impl>();
    producer_impl->fd = fd;
    producer_impl->limits = negotiated.value();
    return VideoProducer{std::move(producer_impl)};
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
    auto negotiated = exchange_limits(fd, options.local_limits, options.timeout);
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
    consumer_impl->assembler =
        std::make_unique<VideoReassembler>(consumer_impl->limits);
    return VideoConsumer{std::move(consumer_impl)};
}

} // namespace aa::ipc
