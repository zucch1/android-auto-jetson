// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aa::ipc {

// AF_UNIX SOCK_SEQPACKET encoded-H.264 channel. This is the wire contract
// proven by the task-10 benchmark spike (tools/aa-ipc-bench) and implemented
// as the production socket in task 22; both share these exact limits and the
// fixed header layout below. The task-10 framing stays the wire authority:
// data records are byte-identical to the spike at the default limits.
//
// Ownership/threading: the producer (receiver media path) writes access units,
// the consumer (decode helper) reassembles them. One socket per consumer;
// queues are bounded and a slow consumer loses data at access-unit granularity
// with drop-to-IDR resync, never unbounded memory. The channel itself is
// caller-driven and thread-free: send on one thread, receive on another, no
// internal threads exist to leak on disconnect.
//
// Error contract: malformed headers, inconsistent fragments, zero-length
// records and oversize records are rejected (transport_malformed_frame /
// transport_oversize_frame) and drive loss/resync. End of stream is the
// producer shutting down its write side, never a zero-length record.
inline constexpr std::size_t kMaxDatagramPayload = 16UL * 1024;
inline constexpr std::size_t kMaxAccessUnitBytes = 4UL * 1024 * 1024;
inline constexpr std::size_t kFrameHeaderBytes = 56;
inline constexpr int kSocketBudgetBytes = 256 * 1024;

// Protocol version carried in every record (task-10 layout, header offset 4).
// A record with a different version is rejected protocol_version_mismatch.
inline constexpr std::uint16_t kVideoProtocolVersion = 1;

// Record magics (big-endian u32 at offset 0). Data records use the task-10
// frame magic; the negotiated-limits exchange uses its own record kind so the
// data-plane framing never changes shape.
inline constexpr std::uint32_t kVideoFrameMagic = 0x41415631U; // "AAV1"
inline constexpr std::uint32_t kVideoLimitsMagic = 0x41414C31U; // "AAL1"

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

// Negotiated channel limits. Each side offers its bounds in the limits record;
// the effective limits are the per-field minimum, which is always within the
// hard wire caps above. offer -> negotiate -> both sides stream with the same
// effective values. Queue model (proven by the task-10 budgets): the bounded
// queues are the kernel socket buffers (SO_SNDBUF/SO_RCVBUF capped at
// socket_budget_bytes) plus one bounded reassembly slot; there is no
// unbounded userspace buffering anywhere.
struct NegotiatedLimits final {
    std::uint32_t max_datagram_payload{kMaxDatagramPayload};
    std::uint32_t max_access_unit_bytes{kMaxAccessUnitBytes};
    std::uint32_t socket_budget_bytes{kSocketBudgetBytes};

    [[nodiscard]] friend constexpr bool operator==(const NegotiatedLimits&,
                                                   const NegotiatedLimits&) noexcept = default;
};

// Limits-exchange record: fixed 32-byte big-endian record sent by each side
// immediately after the peer-credential gate passes, before any data record.
//   offset  0: magic (kVideoLimitsMagic)
//   offset  4: protocol version (u16), record size (u16 = 32)
//   offset  8: kind (u32 = 1)
//   offset 12: max_datagram_payload (u32)
//   offset 16: max_access_unit_bytes (u32)
//   offset 20: socket_budget_bytes (u32)
//   offset 24: reserved (u32 = 0)
//   offset 28: reserved (u32 = 0)
inline constexpr std::size_t kLimitsRecordBytes = 32;
inline constexpr std::uint32_t kLimitsRecordKind = 1;

// One reassembled encoded access unit. `sequence` is the wire sequence of the
// first fragment; `payload` holds exactly `frame_bytes` encoded bytes.
struct VideoAccessUnit final {
    std::uint64_t frame{};
    std::uint64_t sequence{};
    std::uint64_t timestamp_ns{};
    bool idr{};
    std::vector<std::uint8_t> payload{};

    [[nodiscard]] friend bool operator==(const VideoAccessUnit&,
                                         const VideoAccessUnit&) noexcept = default;
};

// Producer-side request handed to the channel; the channel owns fragmentation.
struct VideoSendRequest final {
    std::uint64_t frame{};
    std::uint64_t timestamp_ns{};
    bool idr{};
    std::span<const std::uint8_t> payload{};
};

// Pure wire codec (no sockets). Every decode failure is a typed error; no
// path allocates beyond the caller-supplied spans.
void encode_frame_header(std::span<std::uint8_t> record_prefix,
                         const VideoFrameHeader& header) noexcept;
[[nodiscard]] core::Result<VideoFrameHeader>
decode_frame_header(std::span<const std::uint8_t> record, const NegotiatedLimits& limits) noexcept;

void encode_limits_record(std::array<std::uint8_t, kLimitsRecordBytes>& record,
                          const NegotiatedLimits& limits) noexcept;
[[nodiscard]] core::Result<NegotiatedLimits>
decode_limits_record(std::span<const std::uint8_t> record) noexcept;

// Effective limits = per-field minimum of both offers (already hard-bounded by
// decode). Documented in docs/video/video-socket.md.
[[nodiscard]] core::Result<NegotiatedLimits>
negotiate_limits(const NegotiatedLimits& local, const NegotiatedLimits& peer) noexcept;

// SO_PEERCRED policy: the peer must run as the expected UID (production passes
// the process UID = same-UID only). Fails closed with
// transport_unauthorized_peer on any mismatch.
[[nodiscard]] core::Result<PeerCredentials>
authorize_peer(const PeerCredentials& peer, std::uint32_t expected_uid) noexcept;

} // namespace aa::ipc
