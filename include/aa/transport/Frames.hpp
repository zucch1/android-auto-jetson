// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aa::transport {

// AAP frame wire format (task 16). One frame is
//   [channel:1][flags:1][size:2 or 6][payload:size]
// where `flags` packs the frame type (low 2 bits), the control/message-kind
// bit and the encryption bit, and `size` is big-endian. This header re-states
// the pinned SDK's Messenger layout byte-for-byte but parses it by explicit
// big-endian byte assembly - never an unaligned reinterpret cast - so untrusted
// input is read safely.
//
// The frame channel is a raw wire byte (0..255). It is NOT restricted to the
// SDK's static ChannelId ordinals: dynamic per-session service channels carry
// arbitrary channel bytes and must survive framing untouched.
//
// HARD BOUNDARY (tests/architecture/boundary_scan.py): no AASDK, protobuf or
// OpenSSL type appears here. TLS record sealing is a private adapter concern
// behind the PayloadCipher interface in Framing.hpp.

// Frame type, packed in flags bits 0-1. BULK is a single complete frame;
// FIRST/MIDDLE/LAST are the ordered fragments of one oversized message.
enum class FrameType : std::uint8_t { middle = 0x00, first = 0x01, last = 0x02, bulk = 0x03 };

// Message kind, flags bit 2.
enum class MessageKind : std::uint8_t { specific = 0x00, control = 0x04 };

// Payload cipher, flags bit 3. When `encrypted` the payload is exactly one TLS
// record and the size field counts its full ciphertext (record overhead
// included); the FIRST total still counts plaintext bytes only.
enum class Encryption : std::uint8_t { plain = 0x00, encrypted = 0x08 };

// Semantic bounds. The plaintext chunk cap is the pinned SDK's split threshold;
// an encrypted frame's wire payload is one TLS record larger (overhead included).
inline constexpr std::size_t kPlainChunkBytes = 16384;
inline constexpr std::size_t kTlsRecordOverheadBytes = 29;
inline constexpr std::size_t kMaxWireFrameBytes = kPlainChunkBytes + kTlsRecordOverheadBytes;
inline constexpr std::size_t kMaxMessageBytes = std::size_t{4} * 1024u * 1024u;
inline constexpr std::size_t kMaxQueuedBytes = std::size_t{8} * 1024u * 1024u;

inline constexpr std::size_t kFrameHeaderBytes = 2;
inline constexpr std::size_t kFrameSizeShortBytes = 2;
inline constexpr std::size_t kFrameSizeExtendedBytes = 6;

// Packed frame header. `channel` is the raw wire byte.
struct FrameHeader final {
    std::uint8_t channel{};
    FrameType type{FrameType::middle};
    MessageKind kind{MessageKind::specific};
    Encryption encryption{Encryption::plain};

    [[nodiscard]] friend constexpr bool operator==(const FrameHeader&,
                                                   const FrameHeader&) noexcept = default;
};

// A complete decoded frame. `total_message_bytes` is meaningful only for FIRST
// frames (the extended size field); it is zero otherwise. `payload` is the raw
// wire payload (ciphertext when the frame is encrypted).
struct Frame final {
    FrameHeader header{};
    std::uint32_t total_message_bytes{};
    std::vector<std::byte> payload{};

    [[nodiscard]] friend bool operator==(const Frame&, const Frame&) = default;
};

// Serialize one frame. `payload` is the wire payload (already sealed when the
// header marks it encrypted) and `total_message_bytes` is written only for a
// FIRST frame. Rejects an oversize payload or total with the typed bounds error.
[[nodiscard]] core::Result<std::vector<std::byte>>
encode_frame(const FrameHeader& header, std::span<const std::byte> payload,
             std::uint32_t total_message_bytes);

// Parse one complete frame's bytes by explicit big-endian assembly. Rejects a
// short buffer, reserved flag bits, a size field that disagrees with the buffer
// length, and an oversize payload/total - each with its typed error.
[[nodiscard]] core::Result<Frame> decode_frame(std::span<const std::byte> frame_bytes);

// Incremental byte-stream -> complete-frame splitter. Feeds arbitrary chunks
// (as a socket or USB endpoint delivers them) and yields whole frames once
// their bytes arrive. Bounds each frame before buffering its payload, so a
// hostile length can never force an unbounded allocation. On the first typed
// error the parser is closed and every later feed re-returns transport_closed.
class FrameStreamParser final {
public:
    // Return up to kMaxQueuedBytes of frames, retaining at most that many bytes
    // of multi-frame remainder. feed({}) drains retained complete frames. A
    // larger remainder fails with queue_full and discards all retained input.
    [[nodiscard]] core::Result<std::vector<std::vector<std::byte>>>
    feed(std::span<const std::byte> bytes);

    [[nodiscard]] bool closed() const noexcept { return closed_; }

    void close() noexcept {
        closed_ = true;
        std::vector<std::byte>{}.swap(buffer_);
    }

private:
    std::vector<std::byte> buffer_{};
    bool closed_{false};
};

} // namespace aa::transport
