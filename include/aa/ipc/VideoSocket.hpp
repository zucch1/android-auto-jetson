// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Time.hpp>

#include <cstddef>
#include <cstdint>

namespace aa::ipc {

// AF_UNIX SOCK_SEQPACKET encoded-H.264 channel. This is the wire contract
// proven by the task-10 benchmark spike (tools/aa-ipc-bench) and implemented
// as the production socket in task 22; both share these exact limits and the
// fixed header layout below.
//
// Ownership/threading: the producer (receiver media path) writes access units,
// the consumer (decode helper) reassembles them. One socket per consumer;
// queues are bounded and a slow consumer loses data at access-unit granularity
// with drop-to-IDR resync, never unbounded memory.
//
// Error contract: malformed headers, inconsistent fragments, zero-length
// records and oversize records are rejected (transport_malformed_frame /
// transport_oversize_frame) and drive loss/resync. End of stream is the
// producer shutting down its write side, never a zero-length record.
inline constexpr std::size_t kMaxDatagramPayload = 16UL * 1024;
inline constexpr std::size_t kMaxAccessUnitBytes = 4UL * 1024 * 1024;
inline constexpr std::size_t kFrameHeaderBytes = 56;
inline constexpr int kSocketBudgetBytes = 256 * 1024;

// Fixed 56-byte wire header (field order is the wire layout).
struct VideoFrameHeader final {
    std::uint64_t frame{};         // access-unit id
    std::uint64_t sequence{};      // per-fragment sequence
    std::uint64_t timestamp_ns{};  // monotonic, common A/V clock
    std::uint32_t index{};         // fragment index
    std::uint32_t count{};         // fragment count
    std::uint32_t frame_bytes{};   // full access-unit size
    std::uint32_t payload_bytes{}; // bytes in this fragment
    bool idr{};                    // access unit is an IDR
};

// Peer credential gate: only the registered consumer (UID check via
// SO_PEERCRED) may connect; unauthorized peers are rejected and counted.
struct PeerCredentials final {
    std::uint32_t uid{};
    std::uint32_t pid{};
};

} // namespace aa::ipc
