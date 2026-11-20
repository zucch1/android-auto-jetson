// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include "../crypto_adapter/Crypto.hpp"
#include <array>
#include <charconv>
#include <cstring>
#include <span>
#include <string.h>
#include <string_view>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace aa::helper::nm {
namespace {
struct Fd final {
    int value;
    ~Fd() { if (value >= 0) ::close(value); }
};
int directory(const std::string& path, std::uint32_t owner) {
    if (path.empty() || path.front() != '/') return -1;
    Fd fd{::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
    std::istringstream parts{path.substr(1)};
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == "." || part == "..") return -1;
        const int next = ::openat(fd.value, part.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        struct stat st{};
        if (next < 0) return -1;
        Fd checked{next};
        if (::fstat(next, &st) || (st.st_uid != 0 && st.st_uid != owner) ||
            (st.st_mode & (owner && st.st_uid == owner && !parts.eof() ? 0002 : 0022))) return -1;
        ::close(fd.value);
        fd.value = checked.value;
        checked.value = -1;
    }
    const int result = fd.value;
    fd.value = -1;
    return result;
}
// Reads an owner-only regular file of at most 512 bytes into `buffer`.
core::Result<std::size_t> read_into(int dir, const char* name, std::uint32_t owner, std::span<char, 513> buffer) {
    Fd fd{::openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat st{};
    if (fd.value < 0 || ::fstat(fd.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner ||
        (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size > 512) return Error{ErrorCode::config_invalid};
    const auto size = ::read(fd.value, buffer.data(), buffer.size());
    if (size < 0 || size != st.st_size) return Error{ErrorCode::config_invalid};
    return static_cast<std::size_t>(size);
}
core::Result<std::string> read_file(int dir, const char* name, std::uint32_t owner) {
    std::array<char, 513> data{};
    const auto size = read_into(dir, name, owner, data);
    if (!size) return size.error();
    return std::string{data.data(), size.value()};
}
// #89 L9: a fixed stack buffer for secret file bytes, cleansed on every exit.
template<std::size_t N> struct SecretBuffer final {
    std::array<char, N> bytes{};
    SecretBuffer() = default;
    SecretBuffer(const SecretBuffer&) = delete;
    SecretBuffer& operator=(const SecretBuffer&) = delete;
    ~SecretBuffer() { ::explicit_bzero(bytes.data(), bytes.size()); }
};
bool hex(std::string_view value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
// A fresh SSID ("AA-" + 16 hex) and PSK, the PSK drawn straight into its buffer.
core::Result<Credentials> generate() {
    Credentials result;
    std::array<char, 16> ssid{};
    if (!crypto_adapter::random_hex(ssid) || !crypto_adapter::random_hex(result.psk.digits()))
        return Error{ErrorCode::helper_unavailable};
    result.ssid = "AA-" + std::string{ssid.data(), ssid.size()};
    return result;
}
}
core::Result<Policy> load_policy(const std::string& path, std::uint32_t owner) {
    Fd dir{directory(path, owner)};
    if (dir.value < 0) return Error{ErrorCode::config_invalid};
    auto text = read_file(dir.value, "ap.conf", owner);
    if (!text) return text.error();
    std::istringstream fields{text.value()};
    std::string version, interface, country, confirmed, port, channel, extra;
    if (!(fields >> version >> interface >> country >> confirmed >> port) || (fields >> channel && fields >> extra) ||
        version != "v1" || confirmed != "jurisdiction-confirmed" || country.size() != 2 ||
        country.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos ||
        country == "00" || interface.empty() || interface.size() > 15 ||
        interface.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        return Error{ErrorCode::config_invalid};
    unsigned parsed{};
    const auto number = std::from_chars(port.data(), port.data() + port.size(), parsed);
    if (number.ec != std::errc{} || number.ptr != port.data() + port.size() || parsed < 1024 || parsed > 65535)
        return Error{ErrorCode::config_invalid};
    unsigned ap_channel = 36;
    if (!channel.empty()) {
        const auto value = std::from_chars(channel.data(), channel.data() + channel.size(), ap_channel);
        if (value.ec != std::errc{} || value.ptr != channel.data() + channel.size() || !ap_channel_allowed(ap_channel))
            return Error{ErrorCode::config_invalid};
    }
    return Policy{interface, country, static_cast<std::uint16_t>(parsed),
                  owner ? path + "/state" : "/var/lib/aa-system-helper", owner, ap_channel};
}
core::Result<Credentials> credentials(const Policy& policy, bool rotate) {
    Fd dir{directory(policy.state_directory, policy.owner)};
    struct stat st{};
    if (dir.value < 0 || ::fstat(dir.value, &st) || st.st_uid != policy.owner || (st.st_mode & 0077))
        return Error{ErrorCode::config_invalid};
    struct stat existing{};
    const bool present = ::fstatat(dir.value, "ap.credentials", &existing, AT_SYMLINK_NOFOLLOW) == 0;
    if (present) {
        SecretBuffer<513> saved;
        const auto size = read_into(dir.value, "ap.credentials", policy.owner, saved.bytes);
        if (!size) return size.error();
        const std::string_view text{saved.bytes.data(), size.value()};
        if (text.size() != 85 || text.find('\n') != 19 || !text.starts_with("AA-") ||
            !hex(text.substr(3, 16), 16) || !hex(text.substr(20, 64), 64) || text.back() != '\n')
            return Error{ErrorCode::config_invalid};
        if (!rotate) {
            Credentials result;
            result.ssid = std::string{text.substr(0, 19)};
            std::memcpy(result.psk.digits().data(), text.data() + 20, Psk::kLength);
            return result;
        }
    } else if (errno != ENOENT) return Error{ErrorCode::config_invalid};
    auto generated = generate();
    if (!generated) return generated.error();
    const auto& result = generated.value();
    // "AA-<16 hex>\n<64 hex>\n", composed in place: never a std::string.
    SecretBuffer<85> data;
    std::memcpy(data.bytes.data(), result.ssid.data(), 19);
    data.bytes[19] = '\n';
    std::memcpy(data.bytes.data() + 20, result.psk.c_str(), Psk::kLength);
    data.bytes[84] = '\n';
    // Exclusive staging rejects symlinks and stale/foreign files; no truncation of arbitrary paths.
    Fd output{::openat(dir.value, "ap.credentials.new", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (output.value < 0) return Error{ErrorCode::config_invalid};
    const bool written = ::write(output.value, data.bytes.data(), data.bytes.size()) == static_cast<ssize_t>(data.bytes.size()) &&
                         ::fsync(output.value) == 0;
    if (!written || ::renameat(dir.value, "ap.credentials.new", dir.value, "ap.credentials")) {
        (void)::unlinkat(dir.value, "ap.credentials.new", 0);
        return Error{ErrorCode::helper_unavailable};
    }
    if (::fsync(dir.value)) return Error{ErrorCode::helper_unavailable};
    return generated;
}
core::Result<Credentials> fresh_credentials() { return generate(); }
}
