// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>

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
// Input is low-rate: the kernel queues are capped small so a stalled reader
// cannot grow memory. The wire record is fixed-size, so this is the whole
// budget.
constexpr int kInputSocketBudgetBytes = 32 * 1024;

Error io_error() noexcept { return Error{ErrorCode::transport_io}; }
Error closed_error() noexcept { return Error{ErrorCode::transport_closed}; }

core::Result<void> set_budget(int fd) noexcept {
    const int value = kInputSocketBudgetBytes;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) != 0) {
        return io_error();
    }
    int actual = 0;
    socklen_t length = sizeof(actual);
    if (::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual, &length) != 0 ||
        actual > kInputSocketBudgetBytes * 2) {
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

struct EndpointIdentity final {
    dev_t device{};
    ino_t node{};
};

void unlink_owned_endpoint(const std::string& path, const EndpointIdentity& identity) noexcept {
    struct stat node {};
    if (::lstat(path.c_str(), &node) != 0) {
        return;
    }
    if (!S_ISSOCK(node.st_mode) || node.st_dev != identity.device || node.st_ino != identity.node) {
        return;
    }
    static_cast<void>(::unlink(path.c_str()));
}

enum class EndpointState { stale, occupied };

core::Result<EndpointState> probe_endpoint(const std::string& path) noexcept {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return io_error();
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= kPathMax) {
        ::close(fd);
        return Error{ErrorCode::invalid_argument};
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    EndpointState state = EndpointState::stale;
    const int connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected == 0 ||
        (errno != ECONNREFUSED && errno != ENOENT && errno != ECONNRESET)) {
        state = EndpointState::occupied;
    }
    ::close(fd);
    return state;
}

int make_seqpacket() noexcept {
    return ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

core::Result<EndpointIdentity> bind_path(int fd, const std::string& path) noexcept {
    if (path.size() >= kPathMax) {
        return Error{ErrorCode::invalid_argument};
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return io_error();
    }
    struct stat bound {};
    if (::lstat(path.c_str(), &bound) != 0 || !S_ISSOCK(bound.st_mode)) {
        static_cast<void>(::unlink(path.c_str()));
        return io_error();
    }
    if (::chmod(path.c_str(), 0600) != 0) {
        unlink_owned_endpoint(path, EndpointIdentity{bound.st_dev, bound.st_ino});
        return io_error();
    }
    return EndpointIdentity{bound.st_dev, bound.st_ino};
}

core::Result<void> connect_path(int fd, const std::string& path) noexcept {
    if (path.size() >= kPathMax) {
        return Error{ErrorCode::invalid_argument};
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        // A nonblocking AF_UNIX connect is never established asynchronously:
        // EAGAIN means the listener's accept queue is full (congestion), and
        // retrying later is the caller's next event (one attempt per event).
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Error{ErrorCode::transport_queue_full};
        }
        return Error{ErrorCode::transport_unavailable};
    }
    return {};
}

} // namespace

InputSocketPath production_input_socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string base;
    if (runtime != nullptr && runtime[0] != '\0') {
        base = runtime;
    } else {
        base = "/run/user/";
        base += std::to_string(static_cast<unsigned long long>(::getuid()));
    }
    return input_socket_path_in(base);
}

InputSocketPath input_socket_path_in(const std::string& runtime_dir) {
    std::string value = runtime_dir;
    if (!value.empty() && value.back() != '/') {
        value += '/';
    }
    value += "android-auto-receiver/input.sock";
    return InputSocketPath{std::move(value)};
}

core::Result<void> prepare_input_socket_path(const InputSocketPath& path) {
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
    if (state.value() == EndpointState::occupied) {
        return Error{ErrorCode::transport_unavailable};
    }
    if (::unlink(path.value.c_str()) != 0 && errno != ENOENT) {
        return Error{ErrorCode::transport_unavailable};
    }
    return {};
}

struct InputSender::Impl final {
    int fd{-1};
    InputSocketPath target{};
    std::uint32_t expected_uid{};
    std::array<std::uint8_t, kInputRecordBytes> record{};
    std::uint64_t sent{};
    std::uint64_t dropped{};
    std::optional<ErrorCode> last_failure{};
    bool closed{false};

    Impl() = default;
    ~Impl() { release(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void note_delivery_failure(ErrorCode code) noexcept {
        ++dropped;
        last_failure = code;
    }

    void release() noexcept {
        if (closed) {
            return;
        }
        closed = true;
        disconnect();
    }

    void disconnect() noexcept {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    core::Result<void> ensure_connected() {
        if (fd >= 0) {
            return {};
        }
        if (target.value.empty()) {
            return closed_error();
        }
        const int candidate = make_seqpacket();
        if (candidate < 0) {
            return io_error();
        }
        if (auto connected = connect_path(candidate, target.value); !connected) {
            ::close(candidate);
            return connected.error();
        }
        auto peer = read_peer(candidate);
        if (!peer) {
            ::close(candidate);
            return peer.error();
        }
        if (auto authorized = authorize_peer(peer.value(), expected_uid); !authorized) {
            ::close(candidate);
            return authorized.error();
        }
        if (auto budget = set_budget(candidate); !budget) {
            ::close(candidate);
            return budget.error();
        }
        fd = candidate;
        return {};
    }

    core::Result<void> deliver(const InputEvent& event) {
        if (auto connected = ensure_connected(); !connected) {
            return connected.error();
        }
        encode_input_record(record, event);
        ssize_t written;
        do {
            written = ::send(fd, record.data(), record.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        if (written == static_cast<ssize_t>(record.size())) {
            ++sent;
            return {};
        }
        if (written >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return io_error();
        }
        // Congested send queue: answer with the typed backpressure result
        // IMMEDIATELY - no per-event wait. The caller (the GUI thread) must
        // never accumulate wait time across a burst of events, and this is
        // flow control, so it never maps to a dead-peer retry in send().
        return Error{ErrorCode::transport_queue_full};
    }
};

InputSender::InputSender() : impl_(std::make_unique<Impl>()) {}
InputSender::InputSender(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InputSender::InputSender(InputSender&& other) noexcept : InputSink(), impl_(std::move(other.impl_)) {}
InputSender& InputSender::operator=(InputSender&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}
InputSender::~InputSender() { close(); }

void InputSender::on_input(const InputEvent& event) { static_cast<void>(send(event)); }

void InputSender::attach(InputSocketPath path, std::uint32_t expected_server_uid) {
    if (!impl_ || impl_->closed) {
        return;
    }
    impl_->target = std::move(path);
    impl_->expected_uid = expected_server_uid;
}

core::Result<void> InputSender::send(const InputEvent& event) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    auto attempt = impl_->deliver(event);
    if (attempt) {
        return attempt;
    }
    // Congestion on a live fd is flow control, not a lost peer: keep the
    // connection and count the failed delivery.
    if (impl_->fd < 0 || attempt.error().code() == ErrorCode::transport_queue_full) {
        impl_->note_delivery_failure(attempt.error().code());
        return attempt;
    }
    // The write failed on an established fd: the peer went away mid-session.
    // Drop the fd and retry once so a restarted endpoint still receives this
    // event instead of losing it to a dead descriptor.
    impl_->disconnect();
    attempt = impl_->deliver(event);
    if (!attempt) {
        impl_->note_delivery_failure(attempt.error().code());
    }
    return attempt;
}

void InputSender::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

bool InputSender::closed() const noexcept {
    return !impl_ || impl_->closed || impl_->target.value.empty();
}
std::uint64_t InputSender::sent() const noexcept { return impl_ ? impl_->sent : 0; }
std::uint64_t InputSender::dropped() const noexcept { return impl_ ? impl_->dropped : 0; }
std::optional<ErrorCode> InputSender::last_failure() const noexcept {
    return impl_ ? impl_->last_failure : std::nullopt;
}
int InputSender::native_handle() const noexcept { return impl_ ? impl_->fd : -1; }

struct InputEndpoint::Impl final {
    int fd{-1};
    std::uint64_t rejected{};
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
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            fd = -1;
        }
    }
};

InputEndpoint::InputEndpoint() : impl_(std::make_unique<Impl>()) {}
InputEndpoint::InputEndpoint(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InputEndpoint::InputEndpoint(InputEndpoint&&) noexcept = default;
InputEndpoint& InputEndpoint::operator=(InputEndpoint&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}
InputEndpoint::~InputEndpoint() { close(); }

core::Result<InputEvent> InputEndpoint::receive(core::Milliseconds timeout) {
    if (!impl_ || impl_->closed) {
        return closed_error();
    }
    auto ready = await_readable(impl_->fd, timeout);
    if (!ready) {
        return ready.error();
    }
    // One fixed-size stack record: the receive path never allocates and a
    // larger datagram is typed `transport_oversize_frame` via MSG_TRUNC.
    std::array<std::uint8_t, kInputRecordBytes> buffer{};
    iovec vector{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    ssize_t size;
    do {
        size = ::recvmsg(impl_->fd, &message, 0);
    } while (size < 0 && errno == EINTR);
    if (size < 0) {
        return io_error();
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        ++impl_->rejected;
        return Error{ErrorCode::transport_oversize_frame};
    }
    if (size == 0) {
        pollfd ready{impl_->fd, POLLRDHUP, 0};
        int status;
        do {
            status = ::poll(&ready, 1, 0);
        } while (status < 0 && errno == EINTR);
        if (status >= 0 && (ready.revents & (POLLRDHUP | POLLHUP))) {
            return closed_error();
        }
        ++impl_->rejected;
        return Error{ErrorCode::transport_malformed_frame};
    }
    auto event = decode_input_record(std::span<const std::uint8_t>{buffer.data(),
                                                                  static_cast<std::size_t>(size)});
    if (!event) {
        ++impl_->rejected;
        return event.error();
    }
    return event.value();
}

void InputEndpoint::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

bool InputEndpoint::closed() const noexcept { return !impl_ || impl_->closed; }
std::uint64_t InputEndpoint::rejected() const noexcept { return impl_ ? impl_->rejected : 0; }
int InputEndpoint::native_handle() const noexcept { return impl_ ? impl_->fd : -1; }

struct InputServer::Impl final {
    int fd{-1};
    InputSocketPath path{};
    EndpointIdentity identity{};
    InputServerOptions options{};
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

InputServer::InputServer() : impl_(std::make_unique<Impl>()) {}
InputServer::InputServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InputServer::InputServer(InputServer&&) noexcept = default;
InputServer& InputServer::operator=(InputServer&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}
InputServer::~InputServer() { close(); }

core::Result<InputServer> InputServer::listen(const InputSocketPath& path,
                                              const InputServerOptions& options) {
    if (auto prepared = prepare_input_socket_path(path); !prepared) {
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
    return InputServer{std::move(impl)};
}

core::Result<InputEndpoint> InputServer::accept_sender(core::Milliseconds timeout) {
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
    if (auto budget = set_budget(fd); !budget) {
        ::close(fd);
        return budget.error();
    }
    auto endpoint_impl = std::make_unique<InputEndpoint::Impl>();
    endpoint_impl->fd = fd;
    return InputEndpoint{std::move(endpoint_impl)};
}

void InputServer::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

bool InputServer::closed() const noexcept { return !impl_ || impl_->closed; }
const InputSocketPath& InputServer::path() const noexcept { return impl_->path; }
std::uint64_t InputServer::unauthorized_peers() const noexcept {
    return impl_ ? impl_->unauthorized : 0;
}
int InputServer::native_handle() const noexcept { return impl_ ? impl_->fd : -1; }

} // namespace aa::ipc
