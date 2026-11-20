// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <unistd.h>

namespace aa::diagnostics {
// Descriptor the receivers' JSON stdout is written to. A tool that installs
// aa::tools::StdoutIsolation (tools/json_stdout.hpp; ARCHITECTURE.md, "Process
// stdout") publishes its private copy of the real stdout here; fd 1 is then
// /dev/null. Without isolation (wireless receiver, unit tests) it is fd 1.
inline std::atomic<int> active_json_fd{-1};
inline int json_fd() noexcept {
    const int fd = active_json_fd.load();
    return fd >= 0 ? fd : STDOUT_FILENO;
}
} // namespace aa::diagnostics
