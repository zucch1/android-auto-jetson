// SPDX-License-Identifier: GPL-3.0-or-later
#include "ManagedPeers.hpp"
#include <algorithm>
#include <array>
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
constexpr std::size_t kLimit = 256;
// Unpredictable staging name: a pre-placed or stale file can neither intercept
// the write (O_EXCL) nor block later admissions.
std::string staging_name(const std::string& file) {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device device;
    std::string result = "." + file + "-";
    for (int draw = 0; draw < 4; ++draw) {
        const auto value = static_cast<std::uint32_t>(device()) & 0xFFFFU;
        for (int shift = 12; shift >= 0; shift -= 4) result.push_back(hex[(value >> shift) & 0xFU]);
    }
    return result;
}
int fsync_retry(int fd) {
    int status;
    while ((status = ::fsync(fd)) != 0 && errno == EINTR) {}
    return status;
}
bool write_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto put = ::write(fd, data.data() + done, data.size() - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        done += static_cast<std::size_t>(put);
    }
    return true;
}
}
ManagedPeers::ManagedPeers(std::string path, std::uint32_t owner, std::string file)
    : directory_(std::move(path)), file_(std::move(file)), owner_(owner) {
    const auto reject = [this](const char* why) { reason_ = why; };
    Fd dir{directory(directory_, owner_)};
    if (dir.value < 0) { reject("state directory is missing, not private to its owner, or reached through a symlink"); return; }
    Fd input{::openat(dir.value, file_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (input.value < 0) {
        if (errno == ENOENT) { valid_ = true; return; }
        { reject("state file cannot be opened (symlink or unreadable)"); return; }
    }
    struct stat st{};
    if (::fstat(input.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner_ ||
        (st.st_mode & 07777) != 0600 || st.st_nlink != 1)
        { reject("state file is not a single-link regular 0600 file owned by the helper user"); return; }
    if (st.st_size < 3 || st.st_size > 9220) { reject("state file has an invalid size"); return; }
    std::array<char, 9221> buffer{};
    ssize_t n;
    do { n = ::read(input.value, buffer.data(), buffer.size()); } while (n < 0 && errno == EINTR);
    if (n != st.st_size) { reject("state file could not be read completely"); return; }
    const std::string text{buffer.data(), static_cast<std::size_t>(n)};
    if (!text.starts_with("v1\n") || text.back() != '\n') { reject("state file has an unknown version or a truncated record"); return; }
    std::vector<PeerIdentity> parsed;
    for (std::size_t offset = 3; offset < text.size(); offset += 36) {
        if (offset + 36 > text.size() || text[offset + 17] != ' ' || text[offset + 35] != '\n' || parsed.size() >= kLimit)
            { reject("state file has a malformed or excess record"); return; }
        PeerIdentity peer{text.substr(offset, 17), text.substr(offset + 18, 17)};
        if (!address_valid(peer.address) || !address_valid(peer.adapter) || std::find(parsed.begin(), parsed.end(), peer) != parsed.end())
            { reject("state file has an invalid or duplicate address"); return; }
        parsed.push_back(std::move(peer));
    }
    peers_ = std::move(parsed); valid_ = true;
}
// Revocation authority: the loaded list plus every entry already renamed into
// place. It never depends on whether a later write succeeds.
bool ManagedPeers::contains(const PeerIdentity& peer) const {
    return valid_ && std::find(peers_.begin(), peers_.end(), peer) != peers_.end();
}
// Stages the complete replacement list and renames it into place. Until the rename any failure
// leaves the old file authoritative (staging discarded); past it the new list is the state on disk.
ManagedPeers::Write ManagedPeers::replace(int directory_fd, const std::vector<PeerIdentity>& next) {
    std::string data{"v1\n"};
    for (const auto& saved : next) data += saved.address + " " + saved.adapter + "\n";
    const auto temp = staging_name(file_);
    // Exclusive staging never follows or truncates a foreign/stale file.
    Fd out{::openat(directory_fd, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (out.value < 0) return Write::failed;               // nothing written
    const auto abandon = [&] { (void)::unlinkat(directory_fd, temp.c_str(), 0); return Write::failed; };
    if (::fchmod(out.value, 0600) || !write_all(out.value, data) || fsync_retry(out.value)) return abandon();
    const int closing = out.value; out.value = -1;
    if (::close(closing)) return abandon();
    int renamed;
    do { renamed = ::renameat(directory_fd, temp.c_str(), directory_fd, file_.c_str()); } while (renamed && errno == EINTR);
    if (renamed) return abandon();
    return fsync_retry(directory_fd) ? Write::unsynced : Write::synced;
}
// Admission: true only when the entry is on disk AND its directory entry is
// durable. Every failure refuses this admission only; contains() is unaffected.
bool ManagedPeers::remember(const PeerIdentity& peer) {
    if (!valid_ || !address_valid(peer.address) || !address_valid(peer.adapter)) return false;
    const bool known = contains(peer);
    if (known && durable_) return true;
    if (!known && peers_.size() >= kLimit) return false;
    Fd dir{directory(directory_, owner_)};
    if (dir.value < 0) return false;                       // nothing written
    if (!durable_) {                                       // renamed earlier, directory sync unconfirmed
        if (fsync_retry(dir.value)) return false;
        durable_ = true;
        if (known) return true;
    }
    auto next = peers_;
    next.push_back(peer);
    const auto written = replace(dir.value, next);
    if (written == Write::failed) return false;
    // Past the rename the entry IS the state on disk and cannot roll back, so it
    // counts for revocation even when the directory sync cannot be confirmed.
    peers_ = std::move(next);
    if (written == Write::unsynced) { durable_ = false; return false; }  // refuse this admission; retried next time
    return true;
}
bool ManagedPeers::forget(const PeerIdentity& peer) {
    if (!valid_) return false;
    Fd dir{directory(directory_, owner_)};
    if (!contains(peer)) {
        // Already gone from the list; "gone" is only durable once the rename that removed it is.
        if (durable_) return true;
        if (dir.value < 0 || fsync_retry(dir.value)) return false;
        durable_ = true;
        return true;
    }
    if (dir.value < 0) return false;
    auto next = peers_;
    next.erase(std::find(next.begin(), next.end(), peer));
    const auto written = replace(dir.value, next);
    if (written == Write::failed) return false;
    peers_ = std::move(next);
    if (written == Write::unsynced) { durable_ = false; return false; }  // removed in memory, not yet durable
    durable_ = true;
    return true;
}
}
