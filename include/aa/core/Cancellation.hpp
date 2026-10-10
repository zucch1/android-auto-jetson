// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stop_token>

namespace aa::core {

// Cancellation boundary. C++20 stop tokens are the one project-wide mechanism:
// the session thread owns the source (see SessionThread::token()), transports,
// helpers and long-running operations take the token and observe it. There is
// no second cancellation scheme and no exception-based cancellation.
using CancellationToken = std::stop_token;
using CancellationSource = std::stop_source;

} // namespace aa::core
