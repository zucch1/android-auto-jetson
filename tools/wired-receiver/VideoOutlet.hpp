// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/IdrRecovery.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/protocol/Protocol.hpp>

#include <cerrno>
#include <chrono>
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

    struct IdrRecoveryHooks final {
        std::function<bool()> projecting{};
        std::function<core::Result<void>()> request_idr{};
        std::function<std::chrono::steady_clock::time_point()> clock{};
        std::function<core::Result<void>(bool focus_transition)> request_idr_with_mode{};
        ipc::IdrRecoveryStrategy strategy{ipc::IdrRecoveryStrategy::auto_escalate};
    };

    // `log` carries lifecycle records (endpoint events, final stats). `media_log`
    // (audit L13) carries the periodic, phone-driven records (video_sent,
    // video_dropped, running decode_stats): the receiver binds it to a
    // rate-limited, never-blocking sink. Without one they go to `log` unchanged.
    explicit VideoOutlet(std::function<void(const std::string&)> log, MediaLog media_log = {})
        : log_(std::move(log)), media_log_(std::move(media_log)) {}
    ~VideoOutlet() override { close(); }

    void set_idr_recovery(IdrRecoveryHooks hooks) {
        std::lock_guard lock(mutex_);
        idr_coordinator_.set_strategy(hooks.strategy);
        idr_hooks_ = std::move(hooks);
    }

    [[nodiscard]] bool request_idr_external(std::string_view reason) {
        std::lock_guard lock(mutex_);
        if (!has_idr_request_hook_locked() || !projecting_locked()) {
            return false;
        }
        const auto now = now_locked();
        idr_coordinator_.begin_wait(now);
        return dispatch_idr_request_locked(now, reason);
    }

    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override {
        std::lock_guard lock(mutex_);
        if (!producer_.has_value()) {
            ++dropped_before_consumer_;
            record_drop("no_consumer", frame_cadence(message.header.video_profile));
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
                configure_buffer_locked(*profile_);
            }
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(message.payload.size());
        for (const std::byte byte : message.payload) {
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        }
        const bool is_idr = is_idr_au(
            payload, profile_ ? profile_->codec : protocol::VideoCodec::h264, &av1_reduced_);
        const auto sent = producer_->send_access_unit(ipc::VideoSendRequest{
            ++sequence_, static_cast<std::uint64_t>(message.header.timestamp.count), is_idr,
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
                    idr_coordinator_.reset();
                    had_backpressure_ = false;
                    armed_late_consumer_ = false;
                    producer_installed_at_.reset();
                    record_drop("consumer_disconnected", frame_cadence(profile_));
                    return {};
                }
            }
            return sent.error();
        }
        switch (sent.value()) {
        case ipc::SendStatus::dropped_backpressure:
            had_backpressure_ = true;
            record_drop("backpressure", frame_cadence(profile_));
            maybe_request_idr_locked("backpressure");
            return {};
        case ipc::SendStatus::dropped_waiting_idr: {
            record_drop("waiting_idr", frame_cadence(profile_));
            const bool past_startup_grace =
                producer_installed_at_.has_value() &&
                (now_locked() - *producer_installed_at_) >= ipc::kIdrStartupGrace;
            if (had_backpressure_ || units_ > 0 || replaced_consumer_ ||
                armed_late_consumer_ || past_startup_grace) {
                maybe_request_idr_locked("waiting_idr");
            }
            return {};
        }
        case ipc::SendStatus::sent:
            had_backpressure_ = false;
            replaced_consumer_ = false;
            armed_late_consumer_ = false;
            if (is_idr && idr_coordinator_.waiting()) {
                const auto recovered = idr_coordinator_.on_idr_delivered(now_locked());
                if (recovered.recovered) {
                    log_(ipc::idr_recovered_json("producer", recovered.latency_ms,
                                                 recovered.episode_requests,
                                                 recovered.total_requests));
                }
            }
            break;
        }
        ++units_;
        if (!has_sent_report_ || units_ - last_sent_report_unit_ >= frame_cadence(profile_)) {
            has_sent_report_ = true;
            last_sent_report_unit_ = units_;
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
        if (had_producer_) {
            replaced_consumer_ = true;
        }
        if (dropped_before_consumer_ > 0 || profile_.has_value()) {
            armed_late_consumer_ = true;
            dropped_before_consumer_ = 0;
        }
        had_producer_ = true;
        producer_installed_at_ = now_locked();
        producer_.emplace(std::move(producer));
        if (profile_) {
            configure_buffer_locked(*profile_);
        }
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        emit_drops(true);
        idr_coordinator_.reset();
        had_backpressure_ = false;
        replaced_consumer_ = false;
        armed_late_consumer_ = false;
        dropped_before_consumer_ = 0;
        producer_installed_at_.reset();
        if (producer_.has_value()) {
            producer_->close();
            producer_.reset();
        }
    }

private:
    [[nodiscard]] std::uint64_t frame_cadence(
        const std::optional<protocol::VideoProfile>& profile) const noexcept {
        if (profile && profile->fps > 0) {
            return static_cast<std::uint64_t>(profile->fps);
        }
        if (profile_ && profile_->fps > 0) {
            return static_cast<std::uint64_t>(profile_->fps);
        }
        return 30ULL;
    }

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

    void record_drop(const char* reason, std::uint64_t cadence) {
        ++dropped_;
        if (!has_drop_report_ || dropped_ - last_drop_report_count_ >= cadence) {
            has_drop_report_ = true;
            last_drop_report_count_ = dropped_;
            media("video_dropped", std::string{"{\"event\":\"video_dropped\",\"reason\":\""} + reason +
                                       "\",\"count\":" + std::to_string(dropped_) + "}");
            emit_drops(false);
        }
    }

    [[nodiscard]] std::chrono::steady_clock::time_point now_locked() const {
        return idr_hooks_.clock ? idr_hooks_.clock() : std::chrono::steady_clock::now();
    }

    [[nodiscard]] bool projecting_locked() const {
        return idr_hooks_.projecting ? idr_hooks_.projecting() : false;
    }

    [[nodiscard]] bool has_idr_request_hook_locked() const noexcept {
        return static_cast<bool>(idr_hooks_.request_idr_with_mode) ||
               static_cast<bool>(idr_hooks_.request_idr);
    }

    void configure_buffer_locked(const protocol::VideoProfile& profile) {
        if (!producer_.has_value()) {
            return;
        }
        if (const auto sized = producer_->configure_send_buffer(profile); sized.has_value()) {
            log_(ipc::video_socket_buffer_json("producer", sized.value(), profile));
            if (sized.value().has_warning()) {
                log_(ipc::video_socket_buffer_warning_json("producer", sized.value(), profile));
            }
        }
    }

    void maybe_request_idr_locked(std::string_view reason) {
        if (!has_idr_request_hook_locked()) {
            return;
        }
        const auto now = now_locked();
        idr_coordinator_.begin_wait(now);
        (void)dispatch_idr_request_locked(now, reason);
    }

    [[nodiscard]] bool dispatch_idr_request_locked(std::chrono::steady_clock::time_point now,
                                                   std::string_view reason) {
        const auto prev = idr_coordinator_.snapshot();
        const auto step = idr_coordinator_.try_request_step(now, projecting_locked());
        if (step == ipc::IdrRecoveryStep::none) {
            return false;
        }
        const bool use_transition = (step == ipc::IdrRecoveryStep::focus_transition);
        const auto sent = idr_hooks_.request_idr_with_mode
            ? idr_hooks_.request_idr_with_mode(use_transition)
            : idr_hooks_.request_idr();
        if (!sent) {
            idr_coordinator_.restore_snapshot(prev);
            return false;
        }
        log_(ipc::idr_requested_json("producer", reason));
        if (use_transition) {
            log_(ipc::idr_escalated_json("producer", ipc::idr_recovery_step_name(step), reason,
                                         idr_coordinator_.episode_requests(),
                                         idr_coordinator_.total_requests()));
        }
        return true;
    }

    std::function<void(const std::string&)> log_;
    MediaLog media_log_;
    IdrRecoveryHooks idr_hooks_{};
    ipc::IdrRecoveryCoordinator idr_coordinator_{};
    std::mutex mutex_{};
    std::optional<ipc::VideoProducer> producer_{};
    std::optional<protocol::VideoProfile> profile_;
    std::optional<std::int32_t> session_;
    std::optional<std::uint64_t> generation_;
    bool av1_reduced_{};
    bool had_producer_{};
    bool replaced_consumer_{};
    bool had_backpressure_{};
    bool armed_late_consumer_{};
    std::uint64_t dropped_before_consumer_{};
    std::optional<std::chrono::steady_clock::time_point> producer_installed_at_{};
    std::uint64_t sequence_{};
    std::uint64_t units_{};
    std::uint64_t last_sent_report_unit_{};
    bool has_sent_report_{};
    std::uint64_t dropped_{};
    std::uint64_t last_drop_report_count_{};
    bool has_drop_report_{};
    std::uint64_t exported_observations_{};
};

} // namespace aa::wired_receiver
