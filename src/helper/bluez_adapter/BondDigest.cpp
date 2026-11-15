// SPDX-License-Identifier: GPL-3.0-or-later
#include "BondDigest.hpp"
#include "../crypto_adapter/Crypto.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <random>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace aa::helper::bluez_adapter {
namespace {
struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) ::close(value); }
};
constexpr const char* kKeyFile = "bond-digest.key";

bool address_valid(const std::string& s) {
    if (s.size() != 17) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i % 3 == 2 ? s[i] != ':' : std::string_view{"0123456789ABCDEF"}.find(s[i]) == std::string_view::npos)
            return false;
    return true;
}

int fsync_retry(int fd) {
    int status;
    while ((status = ::fsync(fd)) != 0 && errno == EINTR) {}
    return status;
}

bool write_all(int fd, const unsigned char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const auto put = ::write(fd, data + done, size - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        done += static_cast<std::size_t>(put);
    }
    return true;
}

std::string staging_name() {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device device;
    std::string result = std::string{"."} + kKeyFile + "-";
    for (int draw = 0; draw < 4; ++draw) {
        const auto value = static_cast<std::uint32_t>(device()) & 0xFFFFU;
        for (int shift = 12; shift >= 0; shift -= 4) result.push_back(hex[(value >> shift) & 0xFU]);
    }
    return result;
}

int open_verified_directory(const std::string& path, std::uint32_t owner, bool require_private_leaf) {
    if (path.size() < 2 || path.front() != '/' || path.back() == '/') return -1;
    Fd fd{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (fd.value < 0) return -1;
    std::istringstream parts{path.substr(1)};
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == "." || part == "..") return -1;
        Fd next{::openat(fd.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        struct stat st{};
        if (next.value < 0 || ::fstat(next.value, &st) || (st.st_uid != 0 && st.st_uid != owner)) return -1;
        const bool safe_ancestor = !(st.st_mode & 0022) || (st.st_uid == 0 && (st.st_mode & S_ISVTX));
        const bool ok = (parts.eof() && require_private_leaf)
                            ? (st.st_uid == owner && (st.st_mode & 0777) == 0700)
                            : safe_ancestor;
        if (!ok) return -1;
        ::close(fd.value);
        fd.value = next.value;
        next.value = -1;
    }
    const int result = fd.value;
    fd.value = -1;
    return result;
}

std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool valid_hex_key(std::string_view s) {
    // BlueZ LinkKey and LTK values are 128 bits. Reject truncated or excess material.
    if (s.size() != 32) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

void append_lower_hex(std::string& dst, std::string_view hex) {
    for (char c : hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        dst.push_back(c);
    }
}
} // namespace

BondDigest::BondDigest(std::string state_directory, std::uint32_t owner, std::string bluetooth_directory)
    : state_directory_(std::move(state_directory)),
      bluetooth_directory_(bluetooth_directory.empty()
                               ? (owner == 0 ? "/var/lib/bluetooth" : state_directory_ + "/bluetooth")
                               : std::move(bluetooth_directory)),
      owner_(owner) {
    Fd dir{open_verified_directory(state_directory_, owner_, true)};
    if (dir.value < 0) {
        reason_ = "state directory is missing, not private to its owner, or reached through a symlink";
        return;
    }
    Fd input{::openat(dir.value, kKeyFile, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (input.value < 0) {
        if (errno == ENOENT) {
            valid_ = true;
            (void)ensure_key();
            return;
        }
        reason_ = "bond-digest.key cannot be opened (symlink or unreadable)";
        return;
    }
    struct stat st{};
    if (::fstat(input.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner_ ||
        (st.st_mode & 07777) != 0600 || st.st_nlink != 1) {
        reason_ = "bond-digest.key is not a single-link regular 0600 file owned by the helper user";
        return;
    }
    if (st.st_size != static_cast<off_t>(secret_.size())) {
        reason_ = "bond-digest.key has an invalid size";
        return;
    }
    std::size_t got = 0;
    while (got < secret_.size()) {
        const auto n = ::read(input.value, secret_.data() + got, secret_.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            crypto_adapter::cleanse(secret_.data(), secret_.size());
            reason_ = "bond-digest.key could not be read completely";
            return;
        }
        got += static_cast<std::size_t>(n);
    }
    valid_ = true;
    key_on_disk_ = true;
    key_durable_ = (fsync_retry(dir.value) == 0);
}

BondDigest::~BondDigest() {
    crypto_adapter::cleanse(secret_.data(), secret_.size());
}

bool BondDigest::ensure_key() {
    if (!valid_) return false;
    if (key_durable_) return true;
    Fd dir{open_verified_directory(state_directory_, owner_, true)};
    if (dir.value < 0) return false;
    if (key_on_disk_) {
        if (fsync_retry(dir.value) != 0) return false;
        key_durable_ = true;
        return true;
    }
    std::array<unsigned char, 32> candidate{};
    if (!crypto_adapter::random_bytes(candidate.data(), candidate.size())) return false;
    const auto temp = staging_name();
    Fd out{::openat(dir.value, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (out.value < 0) {
        crypto_adapter::cleanse(candidate.data(), candidate.size());
        return false;
    }
    const auto abandon = [&] {
        (void)::unlinkat(dir.value, temp.c_str(), 0);
        crypto_adapter::cleanse(candidate.data(), candidate.size());
        return false;
    };
    if (::fchmod(out.value, 0600) || !write_all(out.value, candidate.data(), candidate.size()) ||
        fsync_retry(out.value))
        return abandon();
    const int closing = out.value;
    out.value = -1;
    if (::close(closing)) return abandon();
    int renamed;
    do { renamed = ::renameat(dir.value, temp.c_str(), dir.value, kKeyFile); } while (renamed && errno == EINTR);
    if (renamed) return abandon();
    secret_ = candidate;
    crypto_adapter::cleanse(candidate.data(), candidate.size());
    key_on_disk_ = true;
    if (fsync_retry(dir.value) != 0) return false;
    key_durable_ = true;
    return true;
}

std::optional<std::string> BondDigest::compute(const PeerIdentity& peer) {
    if (!valid_ || !address_valid(peer.address) || !address_valid(peer.adapter)) return std::nullopt;
    if (!ensure_key()) return std::nullopt;
    Fd bt_dir{open_verified_directory(bluetooth_directory_, owner_, false)};
    if (bt_dir.value < 0) return std::nullopt;
    Fd adapter_dir{::openat(bt_dir.value, peer.adapter.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat st{};
    if (adapter_dir.value < 0 || ::fstat(adapter_dir.value, &st) ||
        (st.st_uid != 0 && st.st_uid != owner_) || (st.st_mode & 0022))
        return std::nullopt;
    Fd device_dir{::openat(adapter_dir.value, peer.address.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (device_dir.value < 0 || ::fstat(device_dir.value, &st) ||
        (st.st_uid != 0 && st.st_uid != owner_) || (st.st_mode & 0022))
        return std::nullopt;
    Fd info{::openat(device_dir.value, "info", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (info.value < 0 || ::fstat(info.value, &st) || !S_ISREG(st.st_mode) ||
        (st.st_uid != 0 && st.st_uid != owner_) || (st.st_mode & 0022) || st.st_nlink != 1 ||
        st.st_size <= 0 || st.st_size > 65536)
        return std::nullopt;

    std::string raw(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t got = 0;
    while (got < raw.size()) {
        const auto n = ::read(info.value, raw.data() + got, raw.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            crypto_adapter::cleanse(raw.data(), raw.size());
            return std::nullopt;
        }
        got += static_cast<std::size_t>(n);
    }

    std::string_view link_key;
    std::string_view long_term_key;
    std::string_view peripheral_ltk;
    std::string_view slave_ltk;
    std::string_view section;
    bool malformed = false;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto nl = raw.find('\n', pos);
        const auto end = (nl == std::string::npos) ? raw.size() : nl;
        const auto line = trim_ascii(std::string_view{raw.data() + pos, end - pos});
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const auto key = trim_ascii(line.substr(0, eq));
        const auto val = trim_ascii(line.substr(eq + 1));
        if (key != "Key") continue;
        if (section == "LinkKey") {
            if (!valid_hex_key(val)) { malformed = true; break; }
            link_key = val;
        } else if (section == "LongTermKey") {
            if (!valid_hex_key(val)) { malformed = true; break; }
            long_term_key = val;
        } else if (section == "PeripheralLongTermKey") {
            if (!valid_hex_key(val)) { malformed = true; break; }
            peripheral_ltk = val;
        } else if (section == "SlaveLongTermKey") {
            if (!valid_hex_key(val)) { malformed = true; break; }
            slave_ltk = val;
        }
    }

    if (malformed || (link_key.empty() && long_term_key.empty() && peripheral_ltk.empty() && slave_ltk.empty())) {
        crypto_adapter::cleanse(raw.data(), raw.size());
        return std::nullopt;
    }

    std::string canonical;
    canonical.reserve(512);
    canonical.append(peer.adapter);
    canonical.push_back('\n');
    canonical.append(peer.address);
    canonical.append("\nLinkKey=");
    append_lower_hex(canonical, link_key);
    canonical.append("\nLongTermKey=");
    append_lower_hex(canonical, long_term_key);
    canonical.append("\nPeripheralLongTermKey=");
    append_lower_hex(canonical, peripheral_ltk);
    canonical.append("\nSlaveLongTermKey=");
    append_lower_hex(canonical, slave_ltk);
    canonical.push_back('\n');

    crypto_adapter::cleanse(raw.data(), raw.size());
    const auto mac = crypto_adapter::hmac_sha256(secret_.data(), secret_.size(), canonical);
    crypto_adapter::cleanse(canonical.data(), canonical.size());
    if (!mac) return std::nullopt;
    return mac.value();
}
} // namespace aa::helper::bluez_adapter
