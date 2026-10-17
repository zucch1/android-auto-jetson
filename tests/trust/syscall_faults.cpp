// SPDX-License-Identifier: GPL-3.0-or-later
#include "syscall_faults.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <utility>

namespace aa::trust::test::faults {
namespace {

Fault armed = Fault::none;
bool fired = false;
std::mutex observer_mutex;
std::function<void()> observer;

}

void arm(Fault fault) noexcept {
    armed = fault;
    fired = false;
}

void disarm() noexcept {
    armed = Fault::none;
    fired = false;
}

void set_observer(std::function<void()> value) {
    const std::lock_guard lock(observer_mutex);
    observer = std::move(value);
}

void observe() {
    const std::lock_guard lock(observer_mutex);
    if (observer) {
        observer();
    }
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

extern "C" int real_fsync(int) asm("__real_fsync");
extern "C" int real_renameat(int, const char*, int, const char*) asm("__real_renameat");
extern "C" int wrap_fsync(int) asm("__wrap_fsync");
extern "C" int wrap_renameat(int, const char*, int, const char*) asm("__wrap_renameat");

extern "C" int wrap_fsync(int fd) {
    namespace faults = aa::trust::test::faults;
    faults::observe();
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
    return real_fsync(fd);
}

extern "C" int wrap_renameat(int olddirfd, const char* oldpath, int newdirfd,
                               const char* newpath) {
    namespace faults = aa::trust::test::faults;
    faults::observe();
    if (faults::current() == faults::Fault::renameat) {
        errno = EIO;
        return -1;
    }
    return real_renameat(olddirfd, oldpath, newdirfd, newpath);
}
