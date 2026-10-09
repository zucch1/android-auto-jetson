// SPDX-License-Identifier: GPL-3.0-or-later
// AAP frame codec (task 16): serialize and parse the frame wire format by
// explicit big-endian byte assembly. Untrusted input is bounded before any
// buffer grows and every failure is a typed Error - never an unaligned read,
// never an unchecked allocation.

#include <aa/transport/Frames.hpp>

#include <algorithm>

namespace aa::transport {
namespace {

[[nodiscard]] constexpr Error malformed() noexcept {
    return Error{ErrorCode::transport_malformed_frame};
}
[[nodiscard]] constexpr Error oversize() noexcept {
    return Error{ErrorCode::transport_oversize_frame};
}

[[nodiscard]] constexpr std::uint8_t flags_byte(const FrameHeader& header) noexcept {
    return static_cast<std::uint8_t>(header.type) |
           static_cast<std::uint8_t>(header.kind) |
           static_cast<std::uint8_t>(header.encryption);
}

// Frame type from the low 2 flag bits (all four values are representable).
[[nodiscard]] constexpr FrameType type_of(std::uint8_t flags) noexcept {
    return static_cast<FrameType>(flags & 0x03U);
}

[[nodiscard]] constexpr bool is_first(FrameType type) noexcept {
    return type == FrameType::first;
}

// Per-frame payload bound for the given cipher: a plain payload is one
// plaintext chunk; an encrypted payload is one TLS record (overhead included).
[[nodiscard]] constexpr std::size_t payload_bound(Encryption encryption) noexcept {
    return encryption == Encryption::plain ? kPlainChunkBytes : kMaxWireFrameBytes;
}

[[nodiscard]] constexpr std::uint16_t read_be16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8U) |
         static_cast<std::uint16_t>(p[1]));
}

[[nodiscard]] constexpr std::uint32_t read_be32(const std::byte* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
            static_cast<std::uint32_t>(p[3]);
}

// One complete frame never exceeds this on the wire: header + extended size +
// the largest bounded payload. A capped feed can retain a multi-frame remainder
// up to kMaxQueuedBytes; otherwise the tail is at most one incomplete frame.
inline constexpr std::size_t kMaxFrameBytes =
    kFrameHeaderBytes + kFrameSizeExtendedBytes + kMaxWireFrameBytes;

} // namespace

core::Result<std::vector<std::byte>>
encode_frame(const FrameHeader& header, std::span<const std::byte> payload,
             std::uint32_t total_message_bytes) {
    const auto encryption = header.encryption;
    if (payload.size() > payload_bound(encryption)) {
        return oversize();
    }
    if (is_first(header.type) && total_message_bytes > kMaxMessageBytes) {
        return oversize();
    }

    const std::size_t size_width =
        is_first(header.type) ? kFrameSizeExtendedBytes : kFrameSizeShortBytes;
    std::vector<std::byte> out;
    out.reserve(kFrameHeaderBytes + size_width + payload.size());
    out.push_back(static_cast<std::byte>(header.channel));
    out.push_back(static_cast<std::byte>(flags_byte(header)));

    const auto payload_len = static_cast<std::uint16_t>(payload.size());
    out.push_back(static_cast<std::byte>((payload_len >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(payload_len & 0xFFU));
    if (is_first(header.type)) {
        out.push_back(static_cast<std::byte>((total_message_bytes >> 24U) & 0xFFU));
        out.push_back(static_cast<std::byte>((total_message_bytes >> 16U) & 0xFFU));
        out.push_back(static_cast<std::byte>((total_message_bytes >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(total_message_bytes & 0xFFU));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

core::Result<Frame> decode_frame(std::span<const std::byte> frame_bytes) {
    if (frame_bytes.size() < kFrameHeaderBytes) {
        return malformed();
    }
    const std::uint8_t channel = static_cast<std::uint8_t>(frame_bytes[0]);
    const std::uint8_t flags = static_cast<std::uint8_t>(frame_bytes[1]);
    if ((flags & 0xF0U) != 0U) {
        // Reserved flag bits are not part of the wire layout: fail closed.
        return malformed();
    }
    const FrameType type = type_of(flags);
    const auto kind = static_cast<MessageKind>(flags & 0x04U);
    const auto encryption = static_cast<Encryption>(flags & 0x08U);

    const std::size_t size_width = is_first(type) ? kFrameSizeExtendedBytes : kFrameSizeShortBytes;
    if (frame_bytes.size() < kFrameHeaderBytes + size_width) {
        return malformed();
    }
    const std::uint16_t payload_len = read_be16(frame_bytes.data() + kFrameHeaderBytes);
    std::uint32_t total = 0;
    if (is_first(type)) {
        total = read_be32(frame_bytes.data() + kFrameHeaderBytes + kFrameSizeShortBytes);
    }
    if (payload_len > payload_bound(encryption)) {
        return oversize();
    }
    if (is_first(type) && total > kMaxMessageBytes) {
        return oversize();
    }
    const std::size_t expected = kFrameHeaderBytes + size_width + payload_len;
    if (frame_bytes.size() != expected) {
        // The size field must agree with the buffer length exactly.
        return malformed();
    }
    std::vector<std::byte> payload(
        frame_bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes + size_width),
        frame_bytes.begin() + static_cast<std::ptrdiff_t>(expected));
    Frame frame{FrameHeader{channel, type, kind, encryption}, total, std::move(payload)};
    return frame;
}

core::Result<std::vector<std::vector<std::byte>>>
FrameStreamParser::feed(std::span<const std::byte> bytes) {
    if (closed_) {
        return Error{ErrorCode::transport_closed};
    }
    std::vector<std::vector<std::byte>> frames;
    std::size_t out_bytes = 0;
    std::size_t pos = 0;

    // Any failure is terminal and releases retained bytes so a rejected stream
    // never keeps its buffered remainder alive.
    const auto fail = [this](Error error) -> core::Result<std::vector<std::vector<std::byte>>> {
        close();
        return error;
    };

    // Draw up to `n` bytes from the caller's span into the buffer. The whole
    // span is never copied: during incremental parsing only enough to complete
    // one in-progress frame is drawn at a time. On an output-cap the parser may
    // additionally retain a bounded multi-frame remainder (<= kMaxQueuedBytes)
    // for the next feed, so `buffer_` is not limited to a single frame.
    const auto draw = [&](std::size_t n) -> std::size_t {
        const std::size_t take = std::min(n, bytes.size() - pos);
        if (take > 0) {
            buffer_.insert(buffer_.end(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                           bytes.begin() + static_cast<std::ptrdiff_t>(pos + take));
            pos += take;
        }
        return take;
    };

    for (;;) {
        if (buffer_.size() < kFrameHeaderBytes) {
            if (draw(kFrameHeaderBytes - buffer_.size()) == 0) {
                break;
            }
            continue;
        }
        const std::uint8_t flags = static_cast<std::uint8_t>(buffer_[1]);
        if ((flags & 0xF0U) != 0U) {
            return fail(malformed());
        }
        const FrameType type = type_of(flags);
        const auto encryption = static_cast<Encryption>(flags & 0x08U);
        const std::size_t size_width =
            is_first(type) ? kFrameSizeExtendedBytes : kFrameSizeShortBytes;
        if (buffer_.size() < kFrameHeaderBytes + size_width) {
            if (draw(kFrameHeaderBytes + size_width - buffer_.size()) == 0) {
                break;
            }
            continue;
        }
        const std::uint16_t payload_len = read_be16(buffer_.data() + kFrameHeaderBytes);
        if (is_first(type)) {
            // Validate the FIRST total before committing to buffer the payload.
            const std::uint32_t total =
                read_be32(buffer_.data() + kFrameHeaderBytes + kFrameSizeShortBytes);
            if (total > kMaxMessageBytes) {
                return fail(oversize());
            }
        }
        if (payload_len > payload_bound(encryption)) {
            return fail(oversize());
        }
        const std::size_t frame_len = kFrameHeaderBytes + size_width + payload_len;
        if (buffer_.size() < frame_len) {
            if (draw(frame_len - buffer_.size()) == 0) {
                break;
            }
            continue;
        }

        // A complete frame is buffered. Bound the returned queue: emit only up to
        // kMaxQueuedBytes per call and retain the rest (bounded) for the next feed.
        if (out_bytes + frame_len > kMaxQueuedBytes) {
            const std::size_t remaining = bytes.size() - pos;
            if (buffer_.size() + remaining > kMaxQueuedBytes) {
                return fail(Error{ErrorCode::transport_queue_full});
            }
            buffer_.insert(buffer_.end(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end());
            return frames;
        }
        std::vector<std::byte> frame(buffer_.begin(),
                                     buffer_.begin() + static_cast<std::ptrdiff_t>(frame_len));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_len));
        out_bytes += frame_len;
        frames.push_back(std::move(frame));
    }

    // At this point (no output-cap retain) the leftover is at most one
    // in-progress frame; bound it so a malformed length cannot grow it further.
    if (buffer_.size() > kMaxFrameBytes) {
        return fail(oversize());
    }
    return frames;
}

} // namespace aa::transport
