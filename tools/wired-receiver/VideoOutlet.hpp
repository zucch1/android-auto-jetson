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
#include <string_view>
#include <utility>
#include <vector>

namespace aa::wired_receiver {

inline bool is_idr_au(const std::vector<std::uint8_t>& payload, protocol::VideoCodec codec = protocol::VideoCodec::h264, bool* av1_reduced = nullptr) {
    // VP9 uncompressed-header syntax (WebM bitstream specification section 6.2).
    if (codec == protocol::VideoCodec::vp9) {
        std::size_t bit = 0;
        const auto read = [&](unsigned n) -> std::optional<unsigned> {
            if (bit + n > payload.size()*8) { return {}; }
            unsigned value = 0;
            while (n--) { value = (value << 1U) | ((payload[bit/8] >> (7-bit%8)) & 1U); ++bit; }
            return value;
        };
        if (read(2) != 2U) { return false; }
        const auto low = read(1), high = read(1);
        if (!low || !high) { return false; }
        if ((*low | (*high << 1U)) == 3U && !read(1)) { return false; }
        if (read(1) != 0U || read(1) != 0U || !read(2)) { return false; }
        return read(24) == 0x498342U;
    }
    // AV1 low-overhead OBU stream: bounded OBU sizes, sequence header state,
    // then show_existing_frame/frame_type (AV1 specification section 5.9.2).
    if (codec == protocol::VideoCodec::av1) {
        bool reduced = av1_reduced && *av1_reduced;
        for (std::size_t pos = 0; pos < payload.size();) {
            const auto header = payload[pos++];
            if ((header & 0x81U) != 0) { return false; }
            const auto type = (header >> 3U) & 15U;
            if (header & 4U) { if (pos == payload.size()) { return false; } ++pos; }
            std::size_t length = payload.size()-pos;
            if (header & 2U) {
                length = 0;
                bool complete = false;
                for (unsigned i = 0; i < 8 && pos < payload.size(); ++i) {
                    const auto byte = payload[pos++];
                    length |= static_cast<std::size_t>(byte & 127U) << (i*7U);
                    if (!(byte & 128U)) { complete = true; break; }
                }
                if (!complete) { return false; }
            }
            if (length > payload.size()-pos) { return false; }
            if (type == 1 && length) { reduced = (payload[pos] & 8U) != 0; if (av1_reduced) { *av1_reduced = reduced; } }
            if ((type == 3 || type == 6) && length) {
                return reduced || ((payload[pos] & 0x80U) == 0 && ((payload[pos] >> 5U) & 3U) == 0);
            }
            pos += length;
        }
        return false;
    }

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
        if (nal < payload.size() && (codec == protocol::VideoCodec::h264
            ? (payload[nal] & 0x1FU) == 5U
            : ((payload[nal] >> 1U) & 0x3FU) >= 16U && ((payload[nal] >> 1U) & 0x3FU) <= 21U)) {
            return true;
        }
    }
    return false;
}

class VideoOutlet final : public protocol::MessageSink {
public:
    using MediaLog = std::function<void(std::string_view kind, const std::string&)>;

    // `log` carries lifecycle records (endpoint events, final stats). `media_log`
    // (audit L13) carries the periodic, phone-driven records (video_sent,
    // video_dropped, running decode_stats): the receiver binds it to a
    // rate-limited, never-blocking sink. Without one they go to `log` unchanged.
    explicit VideoOutlet(std::function<void(const std::string&)> log, MediaLog media_log = {})
        : log_(std::move(log)), media_log_(std::move(media_log)) {}
    ~VideoOutlet() override { close(); }

    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override {
        std::lock_guard lock(mutex_);
        if (!producer_.has_value()) {
            record_drop("no_consumer");
            return {};
        }
        if (message.header.video_profile != profile_ || message.header.video_session != session_ || message.header.video_generation != generation_) {
            profile_ = message.header.video_profile;
            session_ = message.header.video_session;
            generation_ = message.header.video_generation;
            av1_reduced_ = false;
            if (profile_) {
                log_("{\"event\":\"video_profile\",\"width\":" + std::to_string(profile_->width)
                    + ",\"height\":" + std::to_string(profile_->height) + ",\"fps\":" + std::to_string(profile_->fps)
                    + ",\"codec\":\"" + protocol::codec_name(profile_->codec) + "\"}");
            }
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(message.payload.size());
        for (const std::byte byte : message.payload) {
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        }
        const auto sent = producer_->send_access_unit(ipc::VideoSendRequest{
            ++sequence_, static_cast<std::uint64_t>(message.header.timestamp.count), is_idr_au(payload, profile_ ? profile_->codec : protocol::VideoCodec::h264, &av1_reduced_),
            payload, message.header.video_profile});
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
            media("video_sent", "{\"event\":\"video_sent\",\"units\":" + std::to_string(units_) + "}");
            emit_drops(false);
        }
        return {};
    }

    void install(ipc::VideoProducer producer) {
        std::lock_guard lock(mutex_);
        // send_started fires once per access unit the phone sends (audit L13), so
        // it is media telemetry: rate-limited and never waiting for the sink. Any
        // other endpoint event is a lifecycle record and stays lossless.
        producer.set_observer([this](const ipc::VideoEndpoint& event) {
            auto line = ipc::video_endpoint_json(event);
            if (event.kind == ipc::VideoEndpointKind::send_started) {
                media("video_ipc_send", line);
            } else {
                log_(line);
            }
        });
        if (producer_.has_value()) {
            producer_->close();
        }
        producer_.emplace(std::move(producer));
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        emit_drops(true);
        if (producer_.has_value()) {
            producer_->close();
            producer_.reset();
        }
    }

private:
    void media(std::string_view kind, const std::string& line) {
        if (media_log_) {
            media_log_(kind, line);
        } else {
            log_(line);
        }
    }

    void emit_drops(bool final) {
        if (units_ + dropped_ == 0 || exported_observations_ == units_ + dropped_) {
            return;
        }
        exported_observations_ = units_ + dropped_;
        const auto line = "{\"event\":\"decode_stats\",\"frame_drops\":" + std::to_string(dropped_) + "}";
        if (final) {
            log_(line);
        } else {
            media("decode_stats", line);
        }
    }

    void record_drop(const char* reason) {
        ++dropped_;
        if (dropped_ % 30 == 1) {
            media("video_dropped", std::string{"{\"event\":\"video_dropped\",\"reason\":\""} + reason +
                                       "\",\"count\":" + std::to_string(dropped_) + "}");
            emit_drops(false);
        }
    }

    std::function<void(const std::string&)> log_;
    MediaLog media_log_;
    std::mutex mutex_{};
    std::optional<ipc::VideoProducer> producer_{};
    std::optional<protocol::VideoProfile> profile_;
    std::optional<std::int32_t> session_;
    std::optional<std::uint64_t> generation_;
    bool av1_reduced_{};
    std::uint64_t sequence_{};
    std::uint64_t units_{};
    std::uint64_t dropped_{};
    std::uint64_t exported_observations_{};
};

} // namespace aa::wired_receiver
