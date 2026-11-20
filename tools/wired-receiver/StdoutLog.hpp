// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/diagnostics/StdoutLog.hpp>

namespace aa::wired_receiver {

using aa::diagnostics::StdoutLog;
using aa::diagnostics::StdoutLogScope;
using aa::diagnostics::with_emit_ns;
inline constexpr auto &active_stdout_log = aa::diagnostics::active_stdout_log;

} // namespace aa::wired_receiver
