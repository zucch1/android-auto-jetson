// SPDX-License-Identifier: GPL-3.0-or-later
#include "ManagedPeers.hpp"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace aa::helper::bluez_adapter {
namespace {
struct Fd {
    int value;
    ~Fd() { if (value >= 0) ::close(value); }
};
bool address_valid(const std::string& s) {
    if (s.size() != 17) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i % 3 == 2 ? s[i] != ':' : std::string_view{"0123456789ABCDEF"}.find(s[i]) == std::string_view::npos) return false;
    return true;
}
int directory(const std::string& path, std::uint32_t owner) {
    if (path.size() < 2 || path.front() != '/' || path.back() == '/') return -1;
    Fd fd{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    std::istringstream parts{path.substr(1)};
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == "." || part == "..") return -1;
        Fd next{::openat(fd.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        struct stat st{};
        if (next.value < 0 || ::fstat(next.value, &st) || (st.st_uid != 0 && st.st_uid != owner)) return -1;
        // Sticky root-owned ancestors permit host fixtures under /tmp or /dev/shm.
        // The final state directory must always be private to its owner.
        if (parts.eof() ? (st.st_uid != owner || (st.st_mode & 0777) != 0700) :
            ((st.st_mode & 0022) && !(st.st_uid == 0 && (st.st_mode & S_ISVTX)))) return -1;
        ::close(fd.value); fd.value = next.value; next.value = -1;
    }
    const int result = fd.value; fd.value = -1; return result;
}
constexpr auto name = "managed-bonds";
}
ManagedPeers::ManagedPeers(std::string path, std::uint32_t owner)
    : directory_(std::move(path)), owner_(owner) {
    Fd dir{directory(directory_, owner_)};
    if (dir.value < 0) return;
    Fd file{::openat(dir.value, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (file.value < 0) { valid_ = errno == ENOENT; return; }
    struct stat st{};
    if (::fstat(file.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner_ ||
        (st.st_mode & 07777) != 0600 || st.st_nlink != 1 || st.st_size < 3 || st.st_size > 9220) return;
    std::array<char, 9221> buffer{};
    const auto n = ::read(file.value, buffer.data(), buffer.size());
    if (n != st.st_size) return;
    const std::string text{buffer.data(), static_cast<std::size_t>(n)};
    if (!text.starts_with("v1\n") || text.back() != '\n') return;
    std::vector<PeerIdentity> parsed;
    for (std::size_t offset = 3; offset < text.size(); offset += 36) {
        if (offset + 36 > text.size() || text[offset + 17] != ' ' || text[offset + 35] != '\n' || parsed.size() >= 256) return;
        PeerIdentity peer{text.substr(offset, 17), text.substr(offset + 18, 17)};
        if (!address_valid(peer.address) || !address_valid(peer.adapter) || std::find(parsed.begin(), parsed.end(), peer) != parsed.end()) return;
        parsed.push_back(std::move(peer));
    }
    peers_ = std::move(parsed); valid_ = true;
}
bool ManagedPeers::contains(const PeerIdentity& peer) const {
    return valid_ && std::find(peers_.begin(), peers_.end(), peer) != peers_.end();
}
bool ManagedPeers::remember(const PeerIdentity& peer) {
    if (!valid_ || !address_valid(peer.address) || !address_valid(peer.adapter)) return false;
    if (contains(peer)) return true;
    if (peers_.size() >= 256) return false;
    Fd dir{directory(directory_, owner_)};
    if (dir.value < 0) { valid_ = false; return false; }
    std::string data{"v1\n"};
    for (const auto& saved : peers_) data += saved.address + " " + saved.adapter + "\n";
    data += peer.address + " " + peer.adapter + "\n";
    // Exclusive staging never follows or truncates a foreign/stale file.
    Fd out{::openat(dir.value, "managed-bonds.new", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (out.value < 0) return false;
    const bool written = ::write(out.value, data.data(), data.size()) == static_cast<ssize_t>(data.size()) && ::fsync(out.value) == 0;
    if (!written || ::renameat(dir.value, "managed-bonds.new", dir.value, name)) {
        (void)::unlinkat(dir.value, "managed-bonds.new", 0); return false;
    }
    // A failed directory fsync leaves durability uncertain. Admit nothing more
    // in this process; startup will validate whichever complete record survived.
    if (::fsync(dir.value)) { valid_ = false; return false; }
    peers_.push_back(peer); return true;
}
}
