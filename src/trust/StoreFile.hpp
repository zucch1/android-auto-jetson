// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// File-level primitives for the approved-phone store (task 27): XDG path
// resolution, owner-only permission enforcement and atomic private writes.
// Kept apart from the schema mapper so each concern stays reviewable.
//
// Runtime-file boundary (gate-fix rounds 2-3): every open is symlink-safe
// (O_NOFOLLOW), every check runs against the opened fd (fstat) rather than a
// path lookup, and reads and writes traverse ONLY the verified immediate
// parent descriptor — a symlinked parent is a rejection that mutates nothing
// (no path-based chmod anywhere). Created directories are 0700 at every
// created ancestor (checked fchmod on the creation fd), and the temp file is
// exclusively created (O_CREAT|O_EXCL) under an unpredictable name so a
// pre-placed file or symlink can never intercept the write.

#include <aa/core/Result.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace aa::trust::detail {

inline constexpr const char* kStoreFileName = "approved-phones.json";
inline constexpr std::size_t kMaxStoreBytes = std::size_t{256} * 1024;

// `$XDG_STATE_HOME/android-auto-receiver/approved-phones.json`; falls back to
// `$HOME/.local/state/...` when XDG_STATE_HOME is unset, empty or relative
// (XDG Base Directory spec). config_invalid when neither is resolvable.
[[nodiscard]] core::Result<std::filesystem::path> resolve_default_path();

// Succeeds only when the store path is a regular file owned by the effective
// user with no group/other mode bits, reached THROUGH the verified parent
// descriptor (immediate parent: real directory, effective-user-owned, opened
// O_DIRECTORY|O_NOFOLLOW). A symlink at the store name or at the immediate
// parent is a trust_store_io rejection, never followed.
[[nodiscard]] core::Result<void> ensure_private_file(const std::filesystem::path& path);

// Reads the store file through the verified parent descriptor; an absent file
// yields an empty string (fresh store). Symlinked (store name OR parent),
// foreign-owned, permissive, oversized or unreadable stores are
// trust_store_io.
[[nodiscard]] core::Result<std::string> read_file(const std::filesystem::path& path);

// Atomic replace anchored on the verified store-directory descriptor:
// create missing ancestors at 0700 (checked fchmod on the creation fd, fail
// closed), open the immediate parent O_DIRECTORY|O_NOFOLLOW (a symlinked
// parent rejects BEFORE any mutation), validate ownership via fstat and
// tighten via checked fchmod on that fd to 0700, then exclusively create
// (O_CREAT|O_EXCL) an unpredictably named 0600 temp file there (fchmod
// defeats umask), fsync and rename over `<path>`. A pre-existing symlink at
// the store name is replaced, never followed.
[[nodiscard]] core::Result<void> write_file_private(const std::filesystem::path& path,
                                                    std::string_view content);

} // namespace aa::trust::detail
