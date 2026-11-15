// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include "../crypto_adapter/Crypto.hpp"
#include <array>
#include <charconv>
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
core::Result<std::string> read_file(int dir, const char* name, std::uint32_t owner) {
    Fd fd{::openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat st{};
    if (fd.value < 0 || ::fstat(fd.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner ||
        (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size > 512) return Error{ErrorCode::config_invalid};
    std::array<char, 513> data{};
    const auto size = ::read(fd.value, data.data(), data.size());
    if (size < 0 || size != st.st_size) return Error{ErrorCode::config_invalid};
    return std::string{data.data(), static_cast<std::size_t>(size)};
}
bool hex(const std::string& value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos;
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
        const auto saved = read_file(dir.value, "ap.credentials", policy.owner);
        if (!saved) return saved.error();
        const auto split = saved.value().find('\n');
        if (split != 19 || !saved.value().starts_with("AA-") ||
            !hex(saved.value().substr(3, 16), 16) || saved.value().size() != 85 ||
            !hex(saved.value().substr(20, 64), 64) || saved.value().back() != '\n')
            return Error{ErrorCode::config_invalid};
        if (!rotate) return Credentials{saved.value().substr(0, 19), saved.value().substr(20, 64)};
    } else if (errno != ENOENT) return Error{ErrorCode::config_invalid};
    const auto generated = crypto_adapter::random_ap_secret();
    if (!generated) return generated.error();
    Credentials result{"AA-" + generated.value().substr(0, 16), generated.value().substr(16)};
    const auto data = result.ssid + "\n" + result.psk + "\n";
    // Exclusive staging rejects symlinks and stale/foreign files; no truncation of arbitrary paths.
    Fd output{::openat(dir.value, "ap.credentials.new", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (output.value < 0) return Error{ErrorCode::config_invalid};
    const bool written = ::write(output.value, data.data(), data.size()) == static_cast<ssize_t>(data.size()) &&
                         ::fsync(output.value) == 0;
    if (!written || ::renameat(dir.value, "ap.credentials.new", dir.value, "ap.credentials")) {
        (void)::unlinkat(dir.value, "ap.credentials.new", 0);
        return Error{ErrorCode::helper_unavailable};
    }
    if (::fsync(dir.value)) return Error{ErrorCode::helper_unavailable};
    return result;
}core::Result<Credentials> fresh_credentials() {
    auto generated = crypto_adapter::random_ap_secret();
    if (!generated) return generated.error();
    Credentials result{"AA-" + generated.value().substr(0, 16), generated.value().substr(16)};
    wipe(generated.value());
    return result;
}
}
