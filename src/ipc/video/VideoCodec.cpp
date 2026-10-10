// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/VideoSocket.hpp>

#include <algorithm>
#include <limits>

namespace aa::ipc {
namespace {

void put(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value,
         unsigned size) noexcept {
    for (unsigned i = 0; i < size; ++i) {
        out[offset + i] = static_cast<std::uint8_t>(value >> (8 * (size - i - 1)));
    }
}

template <unsigned size>
std::uint64_t get(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        value = (value << 8) | in[offset + i];
    }
    return value;
}

Error malformed() noexcept { return Error{ErrorCode::transport_malformed_frame}; }
Error oversize() noexcept { return Error{ErrorCode::transport_oversize_frame}; }

} // namespace

void encode_frame_header(std::span<std::uint8_t> record_prefix,
                         const VideoFrameHeader& header) noexcept {
    put(record_prefix, 0, kVideoFrameMagic, 4);
    put(record_prefix, 4, kVideoProtocolVersion, 2);
    put(record_prefix, 6, kFrameHeaderBytes, 2);
    put(record_prefix, 8, header.idr ? 1 : 0, 4);
    put(record_prefix, 12, header.frame, 8);
    put(record_prefix, 20, header.index, 4);
    put(record_prefix, 24, header.count, 4);
    put(record_prefix, 28, header.sequence, 8);
    put(record_prefix, 36, header.timestamp_ns, 8);
    put(record_prefix, 44, header.frame_bytes, 4);
    put(record_prefix, 48, header.payload_bytes, 4);
    put(record_prefix, 52, 0, 4);
}

core::Result<VideoFrameHeader>
decode_frame_header(std::span<const std::uint8_t> record,
                    const NegotiatedLimits& limits) noexcept {
    if (record.size() < kFrameHeaderBytes) {
        return malformed();
    }
    if (record.size() > kFrameHeaderBytes + kMaxDatagramPayload) {
        return oversize();
    }
    if (get<4>(record, 0) != kVideoFrameMagic || get<2>(record, 6) != kFrameHeaderBytes ||
        get<4>(record, 8) > 1 || get<4>(record, 52) != 0) {
        return malformed();
    }
    if (get<2>(record, 4) != kVideoProtocolVersion) {
        return Error{ErrorCode::protocol_version_mismatch};
    }
    VideoFrameHeader header{};
    header.frame = get<8>(record, 12);
    header.index = static_cast<std::uint32_t>(get<4>(record, 20));
    header.count = static_cast<std::uint32_t>(get<4>(record, 24));
    header.sequence = get<8>(record, 28);
    header.timestamp_ns = get<8>(record, 36);
    header.frame_bytes = static_cast<std::uint32_t>(get<4>(record, 44));
    header.payload_bytes = static_cast<std::uint32_t>(get<4>(record, 48));
    header.idr = get<4>(record, 8) == 1;

    const auto datagram_cap = std::min<std::size_t>(limits.max_datagram_payload, kMaxDatagramPayload);
    const auto unit_cap = std::min<std::size_t>(limits.max_access_unit_bytes, kMaxAccessUnitBytes);
    if (header.frame_bytes == 0) {
        return malformed();
    }
    if (header.frame_bytes > unit_cap) {
        return oversize();
    }
    if (header.payload_bytes == 0) {
        return malformed();
    }
    if (header.payload_bytes > datagram_cap) {
        return oversize();
    }
    if (record.size() != kFrameHeaderBytes + header.payload_bytes) {
        return malformed();
    }
    const auto expected_count =
        static_cast<std::uint32_t>((header.frame_bytes + datagram_cap - 1) / datagram_cap);
    if (header.count != expected_count || header.index >= header.count) {
        return malformed();
    }
    const std::size_t offset = static_cast<std::size_t>(header.index) * datagram_cap;
    const auto expected_payload = static_cast<std::uint32_t>(
        std::min<std::size_t>(datagram_cap, header.frame_bytes - offset));
    if (header.payload_bytes != expected_payload) {
        return malformed();
    }
    return header;
}

void encode_limits_record(std::array<std::uint8_t, kLimitsRecordBytes>& record,
                          const NegotiatedLimits& limits) noexcept {
    std::span<std::uint8_t> out{record};
    put(out, 0, kVideoLimitsMagic, 4);
    put(out, 4, kVideoProtocolVersion, 2);
    put(out, 6, kLimitsRecordBytes, 2);
    put(out, 8, kLimitsRecordKind, 4);
    put(out, 12, limits.max_datagram_payload, 4);
    put(out, 16, limits.max_access_unit_bytes, 4);
    put(out, 20, limits.socket_budget_bytes, 4);
    put(out, 24, 0, 4);
    put(out, 28, 0, 4);
}

core::Result<NegotiatedLimits>
decode_limits_record(std::span<const std::uint8_t> record) noexcept {
    if (record.size() != kLimitsRecordBytes) {
        return malformed();
    }
    if (get<4>(record, 0) != kVideoLimitsMagic || get<2>(record, 6) != kLimitsRecordBytes ||
        get<4>(record, 8) != kLimitsRecordKind || get<4>(record, 28) != 0) {
        return malformed();
    }
    if (get<2>(record, 4) != kVideoProtocolVersion) {
        return Error{ErrorCode::protocol_version_mismatch};
    }
    NegotiatedLimits limits{};
    limits.max_datagram_payload = static_cast<std::uint32_t>(get<4>(record, 12));
    limits.max_access_unit_bytes = static_cast<std::uint32_t>(get<4>(record, 16));
    limits.socket_budget_bytes = static_cast<std::uint32_t>(get<4>(record, 20));
    if (get<4>(record, 24) != 0) {
        return malformed();
    }
    if (limits.max_datagram_payload == 0 || limits.max_datagram_payload > kMaxDatagramPayload ||
        limits.max_access_unit_bytes == 0 || limits.max_access_unit_bytes > kMaxAccessUnitBytes ||
        limits.socket_budget_bytes == 0 ||
        limits.socket_budget_bytes > static_cast<std::uint32_t>(kSocketBudgetBytes)) {
        return Error{ErrorCode::protocol_negotiation_failed};
    }
    return limits;
}

core::Result<NegotiatedLimits>
negotiate_limits(const NegotiatedLimits& local, const NegotiatedLimits& peer) noexcept {
    NegotiatedLimits agreed{};
    agreed.max_datagram_payload = std::min(local.max_datagram_payload, peer.max_datagram_payload);
    agreed.max_access_unit_bytes = std::min(local.max_access_unit_bytes, peer.max_access_unit_bytes);
    agreed.socket_budget_bytes = std::min(local.socket_budget_bytes, peer.socket_budget_bytes);
    if (agreed.max_datagram_payload == 0 || agreed.max_access_unit_bytes == 0 ||
        agreed.socket_budget_bytes == 0) {
        return Error{ErrorCode::protocol_negotiation_failed};
    }
    return agreed;
}

core::Result<PeerCredentials> authorize_peer(const PeerCredentials& peer,
                                             std::uint32_t expected_uid) noexcept {
    if (peer.uid != expected_uid) {
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    return peer;
}

} // namespace aa::ipc
