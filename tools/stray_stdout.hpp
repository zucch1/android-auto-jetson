// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>

namespace aa::tools {
// Keeps third-party stdio chatter off a tool's machine-readable stdout.
//
// The receiver and the capability listing write JSON to fd 1. Libraries they
// load write free text through C stdio: the Jetson NVIDIA V4L2 codec library
// (libv4l2_nvvideocodec.so) puts() "Opening in BLOCKING MODE" whenever
// nvv4l2decoder opens, and GLib's g_print() goes the same way. With fd 1 a
// pipe that text sits in the stdio buffer and is flushed later. It lands as a
// non-JSON line in the consumer's stream, and the flush at exit() blocks with
// no bound when the consumer has stopped reading.
//
// While a guard is alive the C stdio `stdout` stream (a plain assignable
// variable in glibc) is stderr, so such text goes to the diagnostic channel.
// File descriptor 1 is untouched: native-fd writers (StdoutLog) still own it,
// and json() is the original stream for deliberate stdio JSON. Construct it
// before any library loads and destroy it after their threads stop. It does
// not catch a library that write()s to fd 1 directly or one that captured the
// stdout stream earlier (std::cout); none on the supported hosts does.
class StrayStdoutToStderr final {
public:
    StrayStdoutToStderr() : json_(stdout) {
        std::fflush(json_);
        stdout = stderr;
    }
    ~StrayStdoutToStderr() {
        std::fflush(stderr);
        stdout = json_;
    }
    StrayStdoutToStderr(const StrayStdoutToStderr &) = delete;
    StrayStdoutToStderr &operator=(const StrayStdoutToStderr &) = delete;

    [[nodiscard]] std::FILE *json() const { return json_; }

private:
    std::FILE *json_;
};
} // namespace aa::tools
