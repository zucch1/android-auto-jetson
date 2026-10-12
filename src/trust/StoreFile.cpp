// SPDX-License-Identifier: GPL-3.0-or-later
#include "StoreFile.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

namespace aa::trust::detail {

namespace {

// Every directory the store creates is owner-only (0700) and every created or
// opened file is 0600; both are enforced on the fd (fchmod/fstat) so the
// ambient umask can never loosen the boundary. All runtime-file operations are
// fd-anchored and symlink-safe: no operation ever follows a symlink at the
// store name, the temp name or the store's immediate parent directory.
constexpr mode_t kFileMode = 0600;
constexpr mode_t kDirMode = 0700;

[[nodiscard]] core::Result<std::filesystem::path> state_home() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        std::filesystem::path candidate{xdg};
        // XDG Base Directory spec: a relative value is invalid and ignored.
        if (candidate.is_absolute()) {
            return candidate;
        }
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return Error{ErrorCode::config_invalid};
    }
    return std::filesystem::path{home} / ".local" / "state";
}

// Fail closed unless `fd` refers to a regular file owned by the effective user
// with no group/other mode bits. Runs against the opened fd (fstat), never the
// path, so a symlink swap cannot redirect the check.
[[nodiscard]] core::Result<void> validate_private_fd(int fd) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        return Error{ErrorCode::trust_store_io};
    }
    if (!S_ISREG(info.st_mode)) {
        return Error{ErrorCode::trust_store_io};
    }
    if (info.st_uid != ::geteuid()) {
        return Error{ErrorCode::trust_store_io};
    }
    if ((info.st_mode & 077) != 0) {
        // Group- or world-accessible store: refuse to trust its contents.
        return Error{ErrorCode::trust_store_io};
    }
    return {};
}

// Opens the store's immediate parent WITHOUT following a symlink
// (O_DIRECTORY|O_NOFOLLOW) and validates ownership on the fd (fstat): a real
// directory owned by the effective user. With `tighten` the directory is
// re-asserted to 0700 via a CHECKED fchmod on the descriptor (fail closed)
// and verified exactly 0700. All store reads and writes traverse only this
// verified descriptor, so a symlinked parent is a rejection that mutates
// nothing (no path-based chmod anywhere).
[[nodiscard]] int open_verified_parent(const std::filesystem::path& parent, bool tighten) {
    const int dirfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) {
        return -1;
    }
    struct stat info {};
    if (::fstat(dirfd, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
        ::close(dirfd);
        return -1;
    }
    if (tighten && (::fchmod(dirfd, kDirMode) != 0 || ::fstat(dirfd, &info) != 0 ||
                    (info.st_mode & 07777) != kDirMode)) {
        ::close(dirfd);
        return -1;
    }
    return dirfd;
}

// Creates every missing ancestor of `parent` at 0700. mkdir applies the
// ambient umask, so the created mode is re-asserted with a checked fchmod on
// the opened creation descriptor (never a path-based chmod; fail closed).
[[nodiscard]] core::Result<void> create_missing_ancestors(const std::filesystem::path& parent) {
    std::filesystem::path current;
    for (const auto& component : parent) {
        current /= component;
        if (current.empty()) {
            continue;
        }
        struct stat info {};
        if (::lstat(current.c_str(), &info) == 0) {
            continue;  // pre-existing component; the store's own dir is verified separately
        }
        if (errno != ENOENT) {
            return Error{ErrorCode::trust_store_io};
        }
        if (::mkdir(current.c_str(), kDirMode) != 0) {
            return Error{ErrorCode::trust_store_io};
        }
        const int fd = ::open(current.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            (void)::rmdir(current.c_str());
            return Error{ErrorCode::trust_store_io};
        }
        const bool tightened = ::fchmod(fd, kDirMode) == 0;
        ::close(fd);
        if (!tightened) {
            (void)::rmdir(current.c_str());
            return Error{ErrorCode::trust_store_io};
        }
    }
    return {};
}

// Unpredictable temp name: a pre-placed file or symlink at any predictable
// name can no longer intercept the write (O_CREAT|O_EXCL rejects collisions).
[[nodiscard]] std::string random_temp_name() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device device;
    std::string name = ".aa-store-";
    for (int draw = 0; draw < 4; ++draw) {
        const auto value = static_cast<std::uint32_t>(device()) & 0xFFFFU;
        for (int shift = 12; shift >= 0; shift -= 4) {
            name.push_back(kHex[(value >> static_cast<unsigned>(shift)) & 0xFU]);
        }
    }
    return name;
}

} // namespace

core::Result<std::filesystem::path> resolve_default_path() {
    auto base = state_home();
    if (!base.has_value()) {
        return base.error();
    }
    return base.value() / "android-auto-receiver" / kStoreFileName;
}

core::Result<void> ensure_private_file(const std::filesystem::path& path) {
    const int dirfd = open_verified_parent(path.parent_path(), false);
    if (dirfd < 0) {
        return Error{ErrorCode::trust_store_io};
    }
    const int fd = ::openat(dirfd, path.filename().c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    ::close(dirfd);
    if (fd < 0) {
        return Error{ErrorCode::trust_store_io};
    }
    auto checked = validate_private_fd(fd);
    ::close(fd);
    return checked;
}

core::Result<std::string> read_file(const std::filesystem::path& path) {
    struct stat probe {};
    if (::lstat(path.c_str(), &probe) != 0) {
        if (errno == ENOENT) {
            return std::string{};  // absent store is the empty store
        }
        return Error{ErrorCode::trust_store_io};
    }
    // The store exists: reach it ONLY through the verified parent descriptor.
    // A symlinked parent is a rejection, never a traversal (fail closed).
    const int dirfd = open_verified_parent(path.parent_path(), false);
    if (dirfd < 0) {
        return Error{ErrorCode::trust_store_io};
    }
    const int fd = ::openat(dirfd, path.filename().c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    ::close(dirfd);
    if (fd < 0) {
        return Error{ErrorCode::trust_store_io};  // includes symlink rejection (ELOOP)
    }
    auto checked = validate_private_fd(fd);
    if (!checked.has_value()) {
        ::close(fd);
        return checked.error();
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        return Error{ErrorCode::trust_store_io};
    }
    const auto size = static_cast<std::size_t>(info.st_size);
    if (size > kMaxStoreBytes) {
        ::close(fd);
        return Error{ErrorCode::trust_store_io};
    }
    std::string out;
    out.resize(size);
    std::size_t filled = 0;
    while (filled < size) {
        const ssize_t got = ::read(fd, out.data() + filled, size - filled);
        if (got <= 0) {
            ::close(fd);
            return Error{ErrorCode::trust_store_io};
        }
        filled += static_cast<std::size_t>(got);
    }
    ::close(fd);
    return out;
}

core::Result<void> write_file_private(const std::filesystem::path& path, std::string_view content) {
    if (content.size() > kMaxStoreBytes) {
        return Error{ErrorCode::trust_store_io};
    }
    const auto parent = path.parent_path();
    const auto filename = path.filename();
    if (parent.empty() || filename.empty()) {
        return Error{ErrorCode::trust_store_io};
    }
    auto created = create_missing_ancestors(parent);
    if (!created.has_value()) {
        return created.error();
    }
    // Anchor every operation below on the verified directory fd (real 0700
    // directory owned by the effective user, tightened via checked fchmod on
    // the fd): no later lookup can be redirected through a swapped-in symlink
    // and a symlinked parent rejects before anything is mutated.
    const int dirfd = open_verified_parent(parent, true);
    if (dirfd < 0) {
        return Error{ErrorCode::trust_store_io};
    }
    const std::string temp_name = random_temp_name();
    const int fd = ::openat(dirfd, temp_name.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kFileMode);
    if (fd < 0) {
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    // Enforce 0600 explicitly: open() applies the ambient umask to new files.
    if (::fchmod(fd, kFileMode) != 0) {
        ::close(fd);
        (void)::unlinkat(dirfd, temp_name.c_str(), 0);
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    if (!validate_private_fd(fd).has_value()) {
        ::close(fd);
        (void)::unlinkat(dirfd, temp_name.c_str(), 0);
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t put = ::write(fd, content.data() + written, content.size() - written);
        if (put <= 0) {
            ::close(fd);
            (void)::unlinkat(dirfd, temp_name.c_str(), 0);
            ::close(dirfd);
            return Error{ErrorCode::trust_store_io};
        }
        written += static_cast<std::size_t>(put);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        (void)::unlinkat(dirfd, temp_name.c_str(), 0);
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    ::close(fd);
    // Atomic replace: rename swaps the verified 0600 regular file over the
    // target inside the verified directory. A pre-existing symlink at the
    // store name is replaced outright (never followed).
    if (::renameat(dirfd, temp_name.c_str(), dirfd, filename.c_str()) != 0) {
        (void)::unlinkat(dirfd, temp_name.c_str(), 0);
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    ::close(dirfd);
    return {};
}

} // namespace aa::trust::detail
