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
// store name, the temp name or any ancestor directory.
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

// fsync with EINTR retry: where a spurious interrupt must not be mistaken
// for a durability failure (the post-rename directory sync reports the
// committed-but-not-durable outcome, so a retryable errno must retry first).
[[nodiscard]] int fsync_interruptible(int fd) {
    int status;
    while ((status = ::fsync(fd)) != 0 && errno == EINTR) {
    }
    return status;
}

// Resolve every component relative to the preceding descriptor. Never normalize
// away a symlink/.. pair: reject .. before any creation or permission change.
// Existing ancestors may be shared (e.g. /tmp); only created directories and
// the final store parent must belong to this user and be private. Reads
// defer the final ownership check until a file is found, preserving the empty
// store semantics for an absent file in a shared directory.
[[nodiscard]] int open_verified_parent(const std::filesystem::path& parent, bool create,
                                       bool allow_foreign = false) {
    for (const auto& component : parent) {
        if (component == "..") {
            errno = EINVAL;
            return -1;
        }
    }
    if (create && parent.is_absolute() && parent.lexically_normal() == "/") {
        errno = EINVAL;
        return -1;
    }
    int current = ::open(parent.is_absolute() ? "/" : ".",
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (current < 0) {
        return -1;
    }
    for (const auto& component : parent.relative_path()) {
        if (component.empty() || component == ".") {
            continue;
        }
        int next = ::openat(current, component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        bool created = false;
        if (next < 0 && errno == ENOENT && create) {
            created = ::mkdirat(current, component.c_str(), kDirMode) == 0;
            if (created || errno == EEXIST) {
                next = ::openat(current, component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            }
        }
        if (next < 0) {
            const int failure = errno;
            ::close(current);
            errno = failure;
            return -1;
        }
        if (created) {
            struct stat info {};
            const bool durable = ::fstat(next, &info) == 0 && S_ISDIR(info.st_mode) &&
                info.st_uid == ::geteuid() && ::fchmod(next, kDirMode) == 0 &&
                ::fstat(next, &info) == 0 && (info.st_mode & 07777) == kDirMode &&
                fsync_interruptible(next) == 0 && fsync_interruptible(current) == 0;
            if (!durable) {
                ::close(next);
                (void)::unlinkat(current, component.c_str(), AT_REMOVEDIR);
                ::close(current);
                errno = EIO;
                return -1;
            }
        }
        ::close(current);
        current = next;
    }
    struct stat info {};
    if (::fstat(current, &info) != 0 || !S_ISDIR(info.st_mode) ||
        (!allow_foreign && info.st_uid != ::geteuid()) ||
        (create && (::fchmod(current, kDirMode) != 0 || ::fstat(current, &info) != 0 ||
                    (info.st_mode & 07777) != kDirMode))) {
        ::close(current);
        errno = EIO;
        return -1;
    }
    return current;
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
    const int dirfd = open_verified_parent(path.parent_path(), false, true);
    if (dirfd < 0) {
        if (errno == ENOENT) { return std::string{}; }
        return Error{ErrorCode::trust_store_io};
    }
    const int fd = ::openat(dirfd, path.filename().c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    const int failure = errno;
    struct stat parent_info {};
    const bool owned = ::fstat(dirfd, &parent_info) == 0 &&
                       parent_info.st_uid == ::geteuid();
    ::close(dirfd);
    if (fd < 0) {
        if (failure == ENOENT) { return std::string{}; }
        return Error{ErrorCode::trust_store_io};  // includes symlink rejection (ELOOP)
    }
    if (!owned) {
        ::close(fd);
        return Error{ErrorCode::trust_store_io};
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
    if (parent.empty() || filename.empty() || filename == "." || filename == "..") {
        return Error{ErrorCode::trust_store_io};
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
    // Replacement protocol: fsync the temp file and CHECKED-close it BEFORE
    // the replacement, then rename over the store and make the rename itself
    // durable by syncing the containing directory. The verified directory
    // descriptor opened above is reused for the rename and the final sync —
    // never reopened by path after the rename.
    if (::fsync(fd) != 0) {
        ::close(fd);
        (void)::unlinkat(dirfd, temp_name.c_str(), 0);
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    // close() releases the descriptor even on error and is never retried on
    // Linux; a checked failure here is still a pre-replacement failure.
    if (::close(fd) != 0) {
        (void)::unlinkat(dirfd, temp_name.c_str(), 0);
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    // Atomic replace: rename swaps the verified 0600 regular file over the
    // target inside the verified directory. A pre-existing symlink at the
    // store name is replaced outright (never followed).
    if (::renameat(dirfd, temp_name.c_str(), dirfd, filename.c_str()) != 0) {
        (void)::unlinkat(dirfd, temp_name.c_str(), 0);
        ::close(dirfd);
        return Error{ErrorCode::trust_store_io};
    }
    // Past the rename the mutation IS the store on disk and cannot roll back
    // (the previous content is gone). A directory-sync failure reports the
    // committed-but-not-durable outcome instead of losing the mutation:
    // trust_store_not_durable keeps the replacement in memory and on disk and
    // only flags crash durability of the new directory entry as unconfirmed.
    const int synced = fsync_interruptible(dirfd);
    ::close(dirfd);
    if (synced != 0) {
        return Error{ErrorCode::trust_store_not_durable};
    }
    return {};
}

} // namespace aa::trust::detail
