// SPDX-License-Identifier: GPL-3.0-or-later
#include "syscall_faults.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

namespace aa::trust::test::faults {
namespace {

Fault armed = Fault::none;
bool fired = false;

}

void arm(Fault fault) noexcept {
    armed = fault;
    fired = false;
}

void disarm() noexcept {
    armed = Fault::none;
    fired = false;
}

[[nodiscard]] Fault current() noexcept { return armed; }

[[nodiscard]] bool consume_eintr_once() noexcept {
    if (fired) {
        return false;
    }
    fired = true;
    return true;
}

} // namespace aa::trust::test::faults

extern "C" int __real_fsync(int);
extern "C" int __real_renameat(int, const char*, int, const char*);

extern "C" int __wrap_fsync(int fd) {
    namespace faults = aa::trust::test::faults;
    const faults::Fault fault = faults::current();
    if (fault != faults::Fault::none) {
        struct stat info {};
        if (::fstat(fd, &info) == 0) {
            const bool is_dir = S_ISDIR(info.st_mode) != 0;
            if (fault == faults::Fault::temp_fsync && !is_dir) {
                errno = EIO;
                return -1;
            }
            if (fault == faults::Fault::dir_fsync && is_dir) {
                errno = EIO;
                return -1;
            }
            if (fault == faults::Fault::dir_fsync_eintr_once && is_dir &&
                faults::consume_eintr_once()) {
                errno = EINTR;
                return -1;
            }
        }
    }
    return __real_fsync(fd);
}

extern "C" int __wrap_renameat(int olddirfd, const char* oldpath, int newdirfd,
                               const char* newpath) {
    namespace faults = aa::trust::test::faults;
    if (faults::current() == faults::Fault::renameat) {
        errno = EIO;
        return -1;
    }
    return __real_renameat(olddirfd, oldpath, newdirfd, newpath);
}
