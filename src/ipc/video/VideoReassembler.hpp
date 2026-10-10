// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/ipc/VideoSocket.hpp>

#include <cstdint>
#include <cstring>
#include <span>

namespace aa::ipc {

class VideoReassembler final {
public:
    explicit VideoReassembler(const NegotiatedLimits& limits) noexcept
        : cap_(limits.max_access_unit_bytes) {
        unit_.payload.resize(cap_);
    }

    VideoReassembler(const VideoReassembler&) = delete;
    VideoReassembler& operator=(const VideoReassembler&) = delete;
    VideoReassembler(VideoReassembler&&) = delete;
    VideoReassembler& operator=(VideoReassembler&&) = delete;

    [[nodiscard]] core::Result<ReceiveStatus> accept(const VideoFrameHeader& header,
                                                     std::span<const std::uint8_t> payload) {
        if (sequence_seen_ && header.sequence != sequence_ + 1) {
            loss();
        }
        sequence_ = header.sequence;
        sequence_seen_ = true;
        if (header.index == 0) {
            if (active_) {
                loss();
            }
            if (frame_seen_ && header.frame <= last_frame_) {
                ++rejected_;
                loss();
                return Error{ErrorCode::transport_malformed_frame};
            }
            last_frame_ = header.frame;
            frame_seen_ = true;
            if (waiting_idr_ && !header.idr) {
                ++suppressed_;
                return ReceiveStatus::waiting;
            }
            current_ = header;
            used_ = 0;
            next_ = 0;
            active_ = true;
            unit_.payload.resize(cap_);
        }
        if (!active_) {
            ++suppressed_;
            return ReceiveStatus::waiting;
        }
        if (header.frame != current_.frame || header.index != next_ ||
            header.count != current_.count || header.frame_bytes != current_.frame_bytes ||
            header.idr != current_.idr || header.timestamp_ns != current_.timestamp_ns) {
            ++rejected_;
            loss();
            return Error{ErrorCode::transport_malformed_frame};
        }
        if (used_ + payload.size() > cap_ || used_ + payload.size() > current_.frame_bytes) {
            ++rejected_;
            loss();
            return Error{ErrorCode::transport_oversize_frame};
        }
        std::memcpy(unit_.payload.data() + used_, payload.data(), payload.size());
        used_ += payload.size();
        ++next_;
        if (next_ != current_.count) {
            return ReceiveStatus::fragment;
        }
        active_ = false;
        unit_.frame = current_.frame;
        unit_.sequence = current_.sequence;
        unit_.timestamp_ns = current_.timestamp_ns;
        unit_.idr = current_.idr;
        unit_.payload.resize(used_);
        if (waiting_idr_) {
            ++resyncs_;
            waiting_idr_ = false;
        }
        return ReceiveStatus::complete;
    }

    void loss() noexcept {
        waiting_idr_ = true;
        active_ = false;
        used_ = 0;
        next_ = 0;
    }

    void reject() noexcept {
        ++rejected_;
        loss();
    }

    [[nodiscard]] const VideoAccessUnit& unit() const noexcept { return unit_; }
    [[nodiscard]] bool waiting_idr() const noexcept { return waiting_idr_; }
    [[nodiscard]] std::uint64_t resyncs() const noexcept { return resyncs_; }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
    [[nodiscard]] std::uint64_t suppressed() const noexcept { return suppressed_; }

private:
    std::size_t cap_;
    VideoAccessUnit unit_{};
    VideoFrameHeader current_{};
    std::size_t used_{};
    std::uint32_t next_{};
    std::uint64_t sequence_{};
    std::uint64_t last_frame_{};
    bool sequence_seen_{};
    bool frame_seen_{};
    bool active_{};
    bool waiting_idr_{true};
    std::uint64_t resyncs_{};
    std::uint64_t rejected_{};
    std::uint64_t suppressed_{};
};

} // namespace aa::ipc
