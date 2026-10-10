// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusConnection.hpp"

#include "BusIo.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace aa::ipc::dbus_adapter {

namespace {

constexpr std::size_t kMaxLineBytes = 512;

struct Endpoint final {
    std::string key;
    std::string value;
};

// First unix: transport of a DBUS_SESSION_BUS_ADDRESS value.
[[nodiscard]] bool session_address(std::string_view address, Endpoint& endpoint) {
    const std::size_t semi = address.find(';');
    const std::string_view first = address.substr(0, semi);
    if (first.compare(0, 5, "unix:") != 0) {
        return false;
    }
    std::string_view rest = first.substr(5);
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        const std::size_t equal = item.find('=');
        if (equal != std::string_view::npos) {
            const auto item_key = item.substr(0, equal);
            if (item_key == "path" || item_key == "abstract") {
                endpoint.key = std::string{item_key};
                endpoint.value = std::string{item.substr(equal + 1)};
                return !endpoint.value.empty();
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest = rest.substr(comma + 1);
    }
    return false;
}

} // namespace

BusConnection::~BusConnection() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

BusConnection::BusConnection(BusConnection&& other) noexcept
    : fd_(other.fd_), next_serial_(other.next_serial_), unique_name_(std::move(other.unique_name_)) {
    other.fd_ = -1;
}

BusConnection& BusConnection::operator=(BusConnection&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        next_serial_ = other.next_serial_;
        unique_name_ = std::move(other.unique_name_);
        other.fd_ = -1;
    }
    return *this;
}

core::Result<ParsedMessage> BusConnection::read_message() {
    std::array<std::uint8_t, 16> head{};
    const auto got_head = bus_io::read_full(fd_, head);
    if (!got_head.has_value()) {
        return got_head.error();
    }
    const auto total = message_size(head);
    if (!total.has_value()) {
        return total.error();
    }
    std::vector<std::uint8_t> bytes(total.value());
    std::copy(head.begin(), head.end(), bytes.begin());
    const auto got_rest =
        bus_io::read_full(fd_, std::span<std::uint8_t>{bytes}.subspan(head.size()));
    if (!got_rest.has_value()) {
        return got_rest.error();
    }
    return parse_message(bytes);
}

core::Result<std::string> BusConnection::read_line() {
    std::string line;
    while (line.size() < kMaxLineBytes) {
        std::array<std::uint8_t, 1> byte{};
        const auto got = bus_io::read_full(fd_, byte);
        if (!got.has_value()) {
            return got.error();
        }
        line.push_back(static_cast<char>(byte[0]));
        if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
            line.resize(line.size() - 2);
            return line;
        }
    }
    return Error{ErrorCode::transport_malformed_frame};
}

core::Result<void> BusConnection::authenticate() {
    const std::string uid = std::to_string(::getuid());
    std::string hex;
    hex.reserve(uid.size() * 2);
    constexpr char kHex[] = "0123456789abcdef";
    for (const char digit : uid) {
        hex.push_back(kHex[(static_cast<unsigned char>(digit) >> 4U) & 0x0FU]);
        hex.push_back(kHex[static_cast<unsigned char>(digit) & 0x0FU]);
    }
    std::string auth;
    auth.push_back('\0');
    auth += "AUTH EXTERNAL ";
    auth += hex;
    auth += "\r\n";
    const auto sent = bus_io::write_full(
        fd_, std::span<const std::uint8_t>{
                 reinterpret_cast<const std::uint8_t*>(auth.data()), auth.size()});
    if (!sent.has_value()) {
        return sent.error();
    }
    const auto line = read_line();
    if (!line.has_value()) {
        return line.error();
    }
    if (line.value().compare(0, 3, "OK ") != 0) {
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    const std::string begin = "BEGIN\r\n";
    return bus_io::write_full(
        fd_, std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(begin.data()),
                                           begin.size()});
}

core::Result<BusConnection> BusConnection::connect_session() {
    const char* environment = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (environment == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    Endpoint endpoint;
    if (!session_address(environment, endpoint)) {
        return Error{ErrorCode::transport_unavailable};
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    socklen_t length = 0;
    if (endpoint.key == "path") {
        if (endpoint.value.size() >= sizeof(address.sun_path)) {
            return Error{ErrorCode::transport_unavailable};
        }
        std::memcpy(address.sun_path, endpoint.value.c_str(), endpoint.value.size() + 1);
        length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint.value.size() + 1);
    } else {
        if (endpoint.value.size() + 1 >= sizeof(address.sun_path)) {
            return Error{ErrorCode::transport_unavailable};
        }
        address.sun_path[0] = '\0';
        std::memcpy(address.sun_path + 1, endpoint.value.data(), endpoint.value.size());
        length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + endpoint.value.size());
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return Error{ErrorCode::transport_io};
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), length) != 0) {
        ::close(fd);
        return Error{ErrorCode::transport_unavailable};
    }
    BusConnection connection;
    connection.fd_ = fd;
    const auto auth = connection.authenticate();
    if (!auth.has_value()) {
        return auth.error();
    }
    MessageWriter hello;
    const auto reply = connection.call("Hello", "", hello);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{ErrorCode::transport_unavailable};
    }
    auto name = reply_string(reply.value());
    if (!name.has_value()) {
        return name.error();
    }
    if (reply.value().destination != name.value()) {
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    connection.unique_name_ = BusName{name.value()};
    return connection;
}

} // namespace aa::ipc::dbus_adapter
