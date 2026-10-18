// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/VideoChannel.hpp>
#include <aa/protocol/Protocol.hpp>

#include <cerrno>
#include <functional>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <utility>
#include <vector>

namespace aa::wired_receiver {

inline bool is_idr_au(const std::vector<std::uint8_t>& payload) {
    for (std::size_t index = 0; index + 4 < payload.size(); ++index) {
        if (payload[index] != 0 || payload[index + 1] != 0) {
            continue;
        }
        std::size_t nal = 0;
        if (payload[index + 2] == 1) {
            nal = index + 3;
        } else if (payload[index + 2] == 0 && payload[index + 3] == 1) {
            nal = index + 4;
        } else {
            continue;
        }
        if (nal < payload.size() && (payload[nal] & 0x1FU) == 5U) {
            return true;
        }
    }
    return false;
}

class VideoOutlet final : public protocol::MessageSink {
public:
    explicit VideoOutlet(std::function<void(const std::string&)> log) : log_(std::move(log)) {}
    ~VideoOutlet() override { close(); }

    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override {
        std::lock_guard lock(mutex_);
        if (!producer_.has_value()) {
            record_drop("no_consumer");
            return {};
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(message.payload.size());
        for (const std::byte byte : message.payload) {
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        }
        const auto sent = producer_->send_access_unit(ipc::VideoSendRequest{
            ++sequence_, static_cast<std::uint64_t>(message.header.timestamp.count), is_idr_au(payload),
            payload});
        if (!sent) {
            // Error erases errno; confirm peer hangup rather than hiding every I/O failure.
            if (sent.error().code() == ErrorCode::transport_io) {
                pollfd peer{producer_->native_handle(), 0, 0};
                int ready;
                do {
                    ready = ::poll(&peer, 1, 0);
                } while (ready < 0 && errno == EINTR);
                if (ready > 0 && (peer.revents & POLLHUP) != 0) {
                    producer_->close();
                    producer_.reset();
                    record_drop("consumer_disconnected");
                    return {};
                }
            }
            return sent.error();
        }
        switch (sent.value()) {
        case ipc::SendStatus::dropped_backpressure:
            record_drop("backpressure");
            return {};
        case ipc::SendStatus::dropped_waiting_idr:
            record_drop("waiting_idr");
            return {};
        case ipc::SendStatus::sent:
            break;
        }
        ++units_;
        if (units_ % 30 == 1) {
            log_("{\"event\":\"video_sent\",\"units\":" + std::to_string(units_) + "}");
            emit_drops();
        }
        return {};
    }

    void install(ipc::VideoProducer producer) {
        std::lock_guard lock(mutex_);
        producer.set_observer([this](const ipc::VideoEndpoint& event) {
            log_(ipc::video_endpoint_json(event));
        });
        if (producer_.has_value()) {
            producer_->close();
        }
        producer_.emplace(std::move(producer));
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        emit_drops();
        if (producer_.has_value()) {
            producer_->close();
            producer_.reset();
        }
    }

private:
    void emit_drops() {
        if (units_ + dropped_ == 0 || exported_observations_ == units_ + dropped_) {
            return;
        }
        exported_observations_ = units_ + dropped_;
        log_("{\"event\":\"decode_stats\",\"frame_drops\":" + std::to_string(dropped_) + "}");
    }

    void record_drop(const char* reason) {
        ++dropped_;
        if (dropped_ % 30 == 1) {
            log_(std::string{"{\"event\":\"video_dropped\",\"reason\":\""} + reason +
                 "\",\"count\":" + std::to_string(dropped_) + "}");
            emit_drops();
        }
    }

    std::function<void(const std::string&)> log_;
    std::mutex mutex_{};
    std::optional<ipc::VideoProducer> producer_{};
    std::uint64_t sequence_{};
    std::uint64_t units_{};
    std::uint64_t dropped_{};
    std::uint64_t exported_observations_{};
};

} // namespace aa::wired_receiver
