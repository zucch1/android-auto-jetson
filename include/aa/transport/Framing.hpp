// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/transport/Frames.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace aa::transport {

// Message-level framing over the AAP frame wire format (task 16): one logical
// message (channel + kind + payload) is fragmented into FIRST/MIDDLE/LAST/BULK
// frames on encode and reassembled on decode, with deterministic typed failures
// for every malformed length, type and reassembly sequence. Per-frame, per
// message and queued memory are all bounded.
//
// PayloadCipher is the TLS seam: when a message is encoded as encrypted, each
// plaintext chunk is sealed into exactly one TLS record and the frame size
// field counts that record's ciphertext; the FIRST total still counts the
// plaintext message. On decode each encrypted frame payload is opened back to a
// plaintext chunk before reassembly. The interface is pure - the concrete TLS
// record adapter lives under src/**/*_adapter/ and never leaks OpenSSL here.

class PayloadCipher {
public:
    virtual ~PayloadCipher() = default;
    PayloadCipher() = default;
    PayloadCipher(const PayloadCipher&) = delete;
    PayloadCipher& operator=(const PayloadCipher&) = delete;
    PayloadCipher(PayloadCipher&&) = delete;
    PayloadCipher& operator=(PayloadCipher&&) = delete;

    // Seal one plaintext chunk into exactly one TLS record (record overhead
    // included in the returned ciphertext). Rejects an inactive cipher.
    [[nodiscard]] virtual core::Result<std::vector<std::byte>>
    seal(std::span<const std::byte> plaintext) = 0;

    // Open one TLS record back to its plaintext chunk. Validates the record
    // length before any arithmetic so a short frame can never underflow - this
    // is the safe replacement for the SDK's hard-coded record-overhead subtract.
    [[nodiscard]] virtual core::Result<std::vector<std::byte>>
    open(std::span<const std::byte> record) = 0;

    // Exact TLS record overhead (bytes) for the negotiated cipher. Valid once
    // is_active() is true.
    [[nodiscard]] virtual std::size_t record_overhead() const noexcept = 0;

    [[nodiscard]] virtual bool is_active() const noexcept = 0;
};

// One logical message: a raw channel byte, its kind and its plaintext payload.
struct Message final {
    std::uint8_t channel{};
    MessageKind kind{MessageKind::specific};
    std::vector<std::byte> payload{};

    [[nodiscard]] friend bool operator==(const Message&, const Message&) = default;
};

// Fragment one message into wire-ready frame byte blobs. `encryption` selects
// plain vs encrypted framing; when encrypted, `cipher` seals each chunk (it may
// be null only for plain). Splits at the plaintext chunk cap and stamps the
// FIRST total with the plaintext message length. Rejects an oversize message or
// an inactive cipher with a typed error.
[[nodiscard]] core::Result<std::vector<std::vector<std::byte>>>
encode_message(const Message& message, Encryption encryption, PayloadCipher* cipher);

// Per-channel message reassembler. push() consumes one complete frame's bytes,
// opens its payload when encrypted, and returns the reassembled message when a
// BULK or LAST frame completes one. Enforces the fragmentation rules (a MIDDLE
// or LAST without a prior FIRST/BULK, a mid-message encryption/kind change, a
// running total that overruns the FIRST total, and any bound overrun) with
// deterministic typed errors. Once it fails it is closed and every later push
// re-returns transport_closed, so a caller can close the transport safely.
class MessageReassembler final {
public:
    // `cipher` is used to open encrypted frame payloads; pass nullptr only for
    // a plain-only stream (an encrypted frame then fails closed).
    explicit MessageReassembler(PayloadCipher* cipher = nullptr) : cipher_(cipher) {}

    // Feed one complete frame's bytes. Returns the completed message when a
    // BULK or LAST frame closes it, or std::nullopt while a fragmented message
    // is still accumulating. A typed error closes the reassembler.
    [[nodiscard]] core::Result<std::optional<Message>>
    push(std::span<const std::byte> frame_bytes);

    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    struct Pending {
        std::uint8_t channel{};
        MessageKind kind{MessageKind::specific};
        Encryption encryption{Encryption::plain};
        std::uint32_t total_message_bytes{};
        std::vector<std::byte> payload{};
    };

    [[nodiscard]] core::Result<std::optional<Message>> fail(Error error);

    PayloadCipher* cipher_;
    // Per-channel in-progress messages, so fragments from different channels
    // may interleave without corrupting each other. The running total of
    // buffered plaintext across all channels is capped by kMaxQueuedBytes.
    std::map<std::uint8_t, Pending> pending_{};
    std::size_t queued_bytes_{};
    bool closed_{false};
};

} // namespace aa::transport
