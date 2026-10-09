// SPDX-License-Identifier: GPL-3.0-or-later
// Message fragmentation and reassembly over AAP frames (task 16). Splits one
// logical message into FIRST/MIDDLE/LAST/BULK frames and reassembles them with
// deterministic typed failures for every malformed length, type and reassembly
// sequence. Per-frame, per-message and queued memory are all bounded.

#include <aa/transport/Framing.hpp>

#include <algorithm>
#include <utility>

namespace aa::transport {
namespace {

[[nodiscard]] constexpr Error malformed() noexcept {
    return Error{ErrorCode::transport_malformed_frame};
}
[[nodiscard]] constexpr Error oversize() noexcept {
    return Error{ErrorCode::transport_oversize_frame};
}

// Seal-or-copy one plaintext chunk into a wire payload.
[[nodiscard]] core::Result<std::vector<std::byte>>
wire_payload(std::span<const std::byte> chunk, Encryption encryption, PayloadCipher* cipher) {
    if (encryption == Encryption::encrypted) {
        return cipher->seal(chunk);
    }
    return std::vector<std::byte>(chunk.begin(), chunk.end());
}

} // namespace

core::Result<std::vector<std::vector<std::byte>>>
encode_message(const Message& message, Encryption encryption, PayloadCipher* cipher) {
    if (message.payload.size() > kMaxMessageBytes) {
        return oversize();
    }
    const bool encrypt = encryption == Encryption::encrypted;
    if (encrypt && (cipher == nullptr || !cipher->is_active())) {
        return Error{ErrorCode::transport_unauthorized_peer};
    }

    const std::span<const std::byte> plain(message.payload);
    const auto total = static_cast<std::uint32_t>(plain.size());
    std::vector<std::vector<std::byte>> frames;

    if (plain.size() <= kPlainChunkBytes) {
        auto payload = wire_payload(plain, encryption, cipher);
        if (!payload) {
            return payload.error();
        }
        FrameHeader header{message.channel, FrameType::bulk, message.kind, encryption};
        auto frame = encode_frame(header, payload.value(), total);
        if (!frame) {
            return frame.error();
        }
        frames.push_back(std::move(frame).value());
        return frames;
    }

    // Oversized messages fragment into >= 2 chunks, so FIRST and LAST are always
    // distinct and both present. Each chunk is at most one plaintext chunk.
    std::size_t offset = 0;
    while (offset < plain.size()) {
        const std::size_t chunk = std::min(kPlainChunkBytes, plain.size() - offset);
        const bool first = offset == 0;
        const bool last = offset + chunk >= plain.size();
        const FrameType type = first ? FrameType::first
                                     : (last ? FrameType::last : FrameType::middle);
        const std::span<const std::byte> chunk_span(plain.data() + offset, chunk);
        auto payload = wire_payload(chunk_span, encryption, cipher);
        if (!payload) {
            return payload.error();
        }
        FrameHeader header{message.channel, type, message.kind, encryption};
        auto frame = encode_frame(header, payload.value(), total);
        if (!frame) {
            return frame.error();
        }
        frames.push_back(std::move(frame).value());
        offset += chunk;
    }
    return frames;
}

core::Result<std::optional<Message>> MessageReassembler::fail(Error error) {
    closed_ = true;
    pending_.clear();
    queued_bytes_ = 0;
    return error;
}

core::Result<std::optional<Message>>
MessageReassembler::push(std::span<const std::byte> frame_bytes) {
    if (closed_) {
        return Error{ErrorCode::transport_closed};
    }
    auto decoded = decode_frame(frame_bytes);
    if (!decoded) {
        return fail(decoded.error());
    }
    const Frame& frame = decoded.value();

    std::vector<std::byte> chunk;
    if (frame.header.encryption == Encryption::encrypted) {
        if (cipher_ == nullptr || !cipher_->is_active()) {
            // Encrypted frame but no active cipher: fail closed.
            return fail(Error{ErrorCode::transport_unauthorized_peer});
        }
        auto opened = cipher_->open(frame.payload);
        if (!opened) {
            return fail(opened.error());
        }
        chunk = std::move(opened).value();
    } else {
        chunk = frame.payload;
    }

    const std::uint8_t channel = frame.header.channel;
    const FrameType type = frame.header.type;

    if (type == FrameType::bulk) {
        if (pending_.count(channel) != 0) {
            return fail(malformed());
        }
        return std::optional<Message>{Message{channel, frame.header.kind, std::move(chunk)}};
    }

    if (type == FrameType::first) {
        if (frame.total_message_bytes == 0) {
            return fail(malformed());
        }
        if (pending_.count(channel) != 0) {
            return fail(malformed());
        }
        if (chunk.size() > frame.total_message_bytes) {
            return fail(malformed());
        }
        if (queued_bytes_ + chunk.size() > kMaxQueuedBytes) {
            return fail(Error{ErrorCode::transport_queue_full});
        }
        Pending p;
        p.channel = channel;
        p.kind = frame.header.kind;
        p.encryption = frame.header.encryption;
        p.total_message_bytes = frame.total_message_bytes;
        p.payload = std::move(chunk);
        queued_bytes_ += p.payload.size();
        pending_.emplace(channel, std::move(p));
        return std::optional<Message>{};
    }

    // MIDDLE or LAST: must continue an in-progress message on this channel.
    auto it = pending_.find(channel);
    if (it == pending_.end()) {
        return fail(malformed());
    }
    Pending& p = it->second;
    if (p.kind != frame.header.kind || p.encryption != frame.header.encryption) {
        return fail(malformed());
    }
    if (p.payload.size() + chunk.size() > p.total_message_bytes) {
        return fail(malformed());
    }
    if (queued_bytes_ + chunk.size() > kMaxQueuedBytes) {
        return fail(Error{ErrorCode::transport_queue_full});
    }
    p.payload.insert(p.payload.end(), chunk.begin(), chunk.end());
    queued_bytes_ += chunk.size();

    if (type == FrameType::middle) {
        return std::optional<Message>{};
    }

    // LAST: the reassembled plaintext must match the FIRST total exactly.
    if (p.payload.size() != p.total_message_bytes) {
        return fail(malformed());
    }
    Message message{channel, p.kind, std::move(p.payload)};
    queued_bytes_ -= message.payload.size();
    pending_.erase(it);
    return std::optional<Message>{std::move(message)};
}

} // namespace aa::transport
