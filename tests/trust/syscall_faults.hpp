// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test-only fault injection for the trust-store replacement protocol. The
// native trust test executable links with `-Wl,--wrap=fsync -Wl,--wrap=renameat`
// so every call from the production code lands in the wrappers below; each
// wrapper falls back to the real syscall unless a fault is armed here (scoped
// activation). File and directory descriptors are told apart with fstat
// inside the wrapper, so a fault targets exactly the temp-file sync or the
// containing-directory sync.

namespace aa::trust::test::faults {

enum class Fault {
    none,
    temp_fsync,            // fail fsync() on non-directory fds (pre-rename)
    dir_fsync,             // fail fsync() on directory fds
    renameat,              // fail renameat() (pre-rename)
    dir_fsync_eintr_once,  // EINTR once on the next directory fsync, then pass
};

void arm(Fault fault) noexcept;
void disarm() noexcept;

class ScopedFault final {
public:
    explicit ScopedFault(Fault fault) noexcept { arm(fault); }
    ~ScopedFault() noexcept { disarm(); }
    ScopedFault(const ScopedFault&) = delete;
    ScopedFault& operator=(const ScopedFault&) = delete;
    ScopedFault(ScopedFault&&) = delete;
    ScopedFault& operator=(ScopedFault&&) = delete;
};

} // namespace aa::trust::test::faults
