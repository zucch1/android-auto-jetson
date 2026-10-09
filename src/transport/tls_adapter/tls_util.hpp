// SPDX-License-Identifier: GPL-3.0-or-later
// Private TLS helpers (task 16): readiness polling and task-8 policy construction.
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/tls/Policy.hpp>
#include <aa/transport/TlsTransport.hpp>

#include <chrono>
#include <cerrno>
#include <algorithm>

#include <poll.h>

namespace aa::transport {

using Deadline = std::chrono::steady_clock::time_point;
inline constexpr int kPollSliceMs = 50;
inline constexpr std::chrono::seconds kHandshakeDeadline{10};

enum class WaitReady { ready, cancelled, timeout, failed };

// Poll `fd` for `events` in short slices, re-checking the cancellation token and
// a whole-operation deadline between slices, so a stalled peer cannot block past
// cancellation or the deadline. Error/hangup revents are surfaced as failed.
[[nodiscard]] inline WaitReady wait_ready(int fd, short events,
                                          const core::CancellationToken& token,
                                          Deadline deadline) {
    for (;;) {
        if (token.stop_requested()) {
            return WaitReady::cancelled;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return WaitReady::timeout;
        }
        ::pollfd descriptor{fd, events, 0};
        const int slice = deadline == Deadline::max() ? kPollSliceMs :
            static_cast<int>(std::min<std::int64_t>(kPollSliceMs,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
        const int ready = ::poll(&descriptor, 1, slice);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return WaitReady::failed;
        }
        if (ready > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return WaitReady::failed;
        }
        if (ready > 0 && (descriptor.revents & events) != 0) {
            return WaitReady::ready;
        }
    }
}

[[nodiscard]] inline aa::tls::Mode to_policy_mode(TlsMode mode) {
    switch (mode) {
    case TlsMode::verified_peer_test_only:
        return aa::tls::Mode::verified_peer_test_only;
    case TlsMode::encryption_only_compatibility:
        return aa::tls::Mode::encryption_only_compatibility;
    }
    throw aa::tls::Error(aa::tls::Failure::configuration);
}

[[nodiscard]] inline aa::tls::Policy make_policy(TlsTransport::ApprovedPredicate approved,
                                                 TlsMode mode) {
    return aa::tls::Policy(std::move(approved), to_policy_mode(mode));
}

} // namespace aa::transport
