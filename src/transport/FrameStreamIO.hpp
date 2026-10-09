// SPDX-License-Identifier: GPL-3.0-or-later
// Shared frame-over-byte-stream I/O for the TCP and USB transports (task 16).
// Splits an inbound byte stream into complete frames and serializes outbound
// frames. The byte source/sink is injected as a read/write callback so the same
// bounded framing logic backs sockets and USB endpoints without duplication.
// Private to src/transport/ - not a public header.
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>
#include <aa/transport/Frames.hpp>

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace aa::transport {

class FrameStreamIO final {
public:
    // Read up to the span's size; 0 means end-of-stream (peer closed). The
    // callback is interruptible and must return core::cancelled once `token`
    // fires (see socket_util / ByteEndpoint).
    using ReadFn = std::function<core::Result<std::size_t>(std::span<std::byte>,
                                                           const core::CancellationToken&)>;
    // Write up to the span's size; returns bytes written (may be partial).
    using WriteFn = std::function<core::Result<std::size_t>(std::span<const std::byte>,
                                                            const core::CancellationToken&)>;

    FrameStreamIO(ReadFn read_fn, WriteFn write_fn)
        : read_fn_(std::move(read_fn)), write_fn_(std::move(write_fn)) {}

    // Validate one complete frame then write all its bytes. A malformed or
    // oversize frame, or a write failure, is a typed error that closes the
    // channel so a caller can close the transport safely.
    [[nodiscard]] core::Result<void> send_frame(std::span<const std::byte> frame,
                                                const core::CancellationToken& token) {
        if (closed_) {
            return Error{ErrorCode::transport_closed};
        }
        if (token.stop_requested()) {
            close();
            return Error{ErrorCode::cancelled};
        }
        auto decoded = decode_frame(frame);
        if (!decoded) {
            close();
            return decoded.error();
        }
        std::size_t written = 0;
        while (written < frame.size()) {
            if (token.stop_requested()) {
                close();
                return Error{ErrorCode::cancelled};
            }
            auto count = write_fn_(frame.subspan(written), token);
            if (!count) {
                close();
                return count.error();
            }
            if (count.value() == 0) {
                close();
                return Error{ErrorCode::transport_closed};
            }
            written += count.value();
        }
        return {};
    }

    // One complete inbound frame, or a typed error / core::cancelled. Reads
    // bytes until the bounded frame stream yields a frame; the parser rejects an
    // oversize frame before buffering it, and each read is interruptible so a
    // stalled peer can never block past cancellation.
    [[nodiscard]] core::Result<std::vector<std::byte>>
    receive_frame(const core::CancellationToken& token) {
        if (closed_) {
            return Error{ErrorCode::transport_closed};
        }
        std::array<std::byte, 4096> buffer{};
        for (;;) {
            if (token.stop_requested()) {
                close();
                return Error{ErrorCode::cancelled};
            }
            if (!ready_.empty()) {
                std::vector<std::byte> frame = std::move(ready_.front());
                ready_.pop_front();
                return frame;
            }
            auto count = read_fn_(buffer, token);
            if (!count) {
                close();
                return count.error();
            }
            if (count.value() == 0) {
                close();
                return Error{ErrorCode::transport_closed};
            }
            auto frames = parser_.feed(std::span<const std::byte>(buffer.data(), count.value()));
            if (!frames) {
                close();
                return frames.error();
            }
            for (auto& frame : frames.value()) {
                ready_.push_back(std::move(frame));
            }
        }
    }

    void close() noexcept {
        closed_ = true;
        ready_.clear();
        parser_.close();
    }

    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    ReadFn read_fn_;
    WriteFn write_fn_;
    FrameStreamParser parser_{};
    std::deque<std::vector<std::byte>> ready_{};
    bool closed_{false};
};

} // namespace aa::transport
