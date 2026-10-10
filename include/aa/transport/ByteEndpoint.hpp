// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace aa::transport {

// USB byte-endpoint abstraction (task 16). This is the byte-stream seam a real
// wired USB/AOA bulk endpoint will implement in task 28; it deliberately has no
// libusb or hardware dependency here. A transport moves AAP frames over any
// ByteEndpoint by splitting and rejoining the byte stream with the shared frame
// codec, so USB framing is exercised today against an in-memory loopback and is
// hardware-independent.
//
// HARD BOUNDARY: no libusb, AASDK or OpenSSL type appears here. The real AOA
// handle belongs to the task-28 adapter under src/**/*_adapter/.
class ByteEndpoint {
public:
    virtual ~ByteEndpoint() = default;
    ByteEndpoint() = default;
    ByteEndpoint(const ByteEndpoint&) = delete;
    ByteEndpoint& operator=(const ByteEndpoint&) = delete;
    ByteEndpoint(ByteEndpoint&&) = delete;
    ByteEndpoint& operator=(ByteEndpoint&&) = delete;

    // Open the endpoint. Idempotent.
    [[nodiscard]] virtual core::Result<void> open() = 0;

    // Read up to `buffer.size()` bytes. A zero return means end-of-stream (the
    // peer closed); a typed error means the endpoint failed and is now closed.
    // The read is interruptible: it returns core::cancelled once `token` fires,
    // so a stalled peer can never block the caller indefinitely.
    [[nodiscard]] virtual core::Result<std::size_t>
    read(std::span<std::byte> buffer, const core::CancellationToken& token) = 0;

    // Write up to data.size() bytes; every syscall/wait must observe token.
    // The bounded loopback rejects a full queue instead of waiting for space.
    [[nodiscard]] virtual core::Result<std::size_t>
    write(std::span<const std::byte> data, const core::CancellationToken& token = {}) = 0;

    virtual void close() noexcept = 0;
};

// A connected in-memory endpoint pair, for tests and for exercising the USB
// framing path without hardware. make_loopback() returns two endpoints joined
// by a bounded byte queue: bytes written on one are read on the other.
struct LoopbackPair final {
    std::unique_ptr<ByteEndpoint> a;
    std::unique_ptr<ByteEndpoint> b;
};

// Create a connected loopback pair. The queue is bounded; a write that would
// exceed the bound fails with transport_queue_full rather than growing forever.
[[nodiscard]] core::Result<LoopbackPair> make_loopback();

} // namespace aa::transport
