// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace aa::transport {

// Transport boundary: a framed-message channel between the receiver and one
// phone. Every send/receive crosses this interface as exactly one complete
// frame; socket bytes, USB transfers and TLS records never leak past it.
//
// Ownership: implementations own their file descriptor / USB handle / SSL
// object exclusively. The session owns the Transport instance and is the only
// caller of open/send/receive/close (session thread); an implementation may
// use an internal I/O thread but must not hand raw handles to anyone.
// Request cancellation from another thread, then join the session operation
// before close/destruction. Arbitrary concurrent close/send/receive is unsupported.
// DNS/connect/listen/accept factories remain synchronous and are not cancellable.
//
// Error contract: all failures surface as aa::Error. Malformed or oversize
// inbound data returns transport_malformed_frame / transport_oversize_frame
// and leaves the channel closed, never partially consumed and never with
// unbounded allocation.
//
// Implementation seam: USB/AOA, TCP and TLS adapters live under
// src/**/*_adapter/ and are the only place external headers (OpenSSL, libusb,
// AASDK transport types) may appear. This public interface stays free of them.
enum class Kind { usb_aoa, tcp, tls };

class Transport {
public:
    virtual ~Transport() = default;
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    [[nodiscard]] virtual Kind kind() const noexcept = 0;

    // Binds this channel to the session cancellation token. Idempotent open.
    virtual core::Result<void> open(core::CancellationToken cancellation) = 0;

    // One complete bounded frame out. USB loopback queue saturation returns
    // transport_queue_full. TCP/TLS backpressure waits for space until cancelled;
    // it does not promise queue_full or a send deadline without a stop request.
    virtual core::Result<void> send(std::span<const std::byte> frame) = 0;

    // One complete frame in, or a typed error. Returns core::cancelled once the
    // bound cancellation token fires; peer EOF returns transport_closed (TLS
    // truncation may return transport_io). Any failure terminally closes it.
    virtual core::Result<std::vector<std::byte>> receive() = 0;

    virtual void close() noexcept = 0;
};

} // namespace aa::transport
