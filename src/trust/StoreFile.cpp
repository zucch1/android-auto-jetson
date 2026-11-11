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

// fsync with EINTR retry: where a spurious interrupt must not be mistaken
// for a durability failure (the post-rename directory sync reports the
// committed-but-not-durable outcome, so a retryable errno must retry first).
[[nodiscard]] int fsync_interruptible(int fd) {
    int status;
    while ((status = ::fsync(fd)) != 0 && errno == EINTR) {
    }
    return status;
}

// Walks EVERY component of `parent` relative to a directory descriptor,
// never by absolute path (audit M11): O_DIRECTORY|O_NOFOLLOW rejects a symlink
// or non-directory at any level, so neither the immediate parent nor any
// ancestor can redirect the store. Once opened, a component stays anchored
// even if its name is later replaced. The final directory must be owned by the
// effective user; with `tighten` it is re-asserted to 0700 via a CHECKED
// fchmod on the descriptor (fail closed) and verified exactly 0700.
//
// With `create`, a missing component is made with mkdirat relative to the
// already-verified containing descriptor (never a path-based mkdir). mkdirat
// applies the ambient umask, so the mode is re-asserted with a checked fchmod
// on the new directory's own descriptor. Each newly created directory entry is
// made durable before anything is written inside it: the new directory and its
// containing directory descriptor are fsynced. This runs pre-replacement, so a
// failure rolls the creation back (unlinkat AT_REMOVEDIR) and fails closed.
[[nodiscard]] int open_verified_parent(const std::filesystem::path& parent, bool tighten,
                                       bool create = false) {
    // Every failure returns -1 with errno describing WHY (preserved across the
    // cleanup close() calls), so read_file() can tell a genuinely missing
    // component (ENOENT: a fresh install) from a rejection. Ownership and mode
    // failures report EACCES, never ENOENT.
    const auto fail = [](int saved_errno, int fd) {
        if (fd >= 0) {
            ::close(fd);
        }
        errno = saved_errno;
        return -1;
    };
    if (parent.empty()) {
        errno = EINVAL;
        return -1;
    }
    int dirfd = ::open(parent.is_absolute() ? "/" : ".",
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) {
        return -1;
    }
    for (const auto& component : parent) {
        if (component.empty() || component == "/" || component == ".") {
            continue;
        }
        int next = ::openat(dirfd, component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && create && errno == ENOENT) {
            if (::mkdirat(dirfd, component.c_str(), kDirMode) != 0) {
                return fail(errno, dirfd);
            }
            next = ::openat(dirfd, component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            const bool durable = next >= 0 && ::fchmod(next, kDirMode) == 0 &&
                                 fsync_interruptible(next) == 0 &&
                                 fsync_interruptible(dirfd) == 0;
            if (!durable) {
                const int saved = errno != 0 ? errno : EIO;
                if (next >= 0) {
                    ::close(next);
                }
                (void)::unlinkat(dirfd, component.c_str(), AT_REMOVEDIR);
                return fail(saved, dirfd);
            }
        }
        if (next < 0) {
            return fail(errno, dirfd);
        }
        ::close(dirfd);
        dirfd = next;
    }
    struct stat info {};
    if (::fstat(dirfd, &info) != 0) {
        return fail(errno, dirfd);
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
        return fail(EACCES, dirfd);
    }
    if (tighten && (::fchmod(dirfd, kDirMode) != 0 || ::fstat(dirfd, &info) != 0 ||
                    (info.st_mode & 07777) != kDirMode)) {
        return fail(EACCES, dirfd);
    }
    return dirfd;
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
    // Verify EVERY directory component first (audit M11), with no path-based
    // pre-check: a symlinked ancestor is trust_store_io even when its target
    // lacks the store file. Only a genuinely missing directory component
    // (ENOENT from the walk: a fresh install) or a missing basename inside the
    // verified directory is the empty store.
    const int dirfd = open_verified_parent(path.parent_path(), false);
    if (dirfd < 0) {
        if (errno == ENOENT) {
            return std::string{};  // state directory not created yet
        }
        return Error{ErrorCode::trust_store_io};
    }
    const int fd = ::openat(dirfd, path.filename().c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    const int open_errno = errno;
    ::close(dirfd);
    if (fd < 0) {
        if (open_errno == ENOENT) {
            return std::string{};  // absent store is the empty store
        }
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
    // Create and open the parent in one descriptor-anchored walk (audit M11):
    // every directory component rejects symlinks, missing components are made
    // durable before use, and every operation below is anchored on the final
    // verified 0700 descriptor, so no later lookup can be redirected through a
    // swapped-in symlink and a symlinked component rejects before mutation
    // below it.
    const int dirfd = open_verified_parent(parent, true, true);
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
