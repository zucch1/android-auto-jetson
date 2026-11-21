// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/IdrRecovery.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/protocol/Protocol.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <deque>
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

    [[nodiscard]] bool idr_dispatching() const noexcept {
        return idr_dispatching_.load(std::memory_order_acquire);
    }

    // Wireless playout (credit window + playout, 2026-11-20): video is held this long after it arrives
    // before it is forwarded, matching the audio playout buffer so audio and
    // video still leave the receiver together. The frame keeps its arrival
    // timestamp, so the display's video_presented latency includes the delay
    // exactly as audio_write_completed's does. Zero (wired) forwards at once.
    void set_presentation_delay(std::chrono::nanoseconds delay) {
        std::lock_guard lock(mutex_);
        delay_ = std::max(std::chrono::nanoseconds{0}, delay);
    }

    [[nodiscard]] std::size_t delayed_units() const {
        std::lock_guard lock(mutex_);
        return delayed_.size();
    }

    [[nodiscard]] bool has_pending() const {
        std::lock_guard lock(mutex_);
        return !pending_queue_.empty() || !delayed_.empty();
    }

    [[nodiscard]] std::size_t pending_units() const {
        std::lock_guard lock(mutex_);
        return pending_queue_.size();
    }

    [[nodiscard]] std::size_t pending_bytes() const {
        std::lock_guard lock(mutex_);
        return pending_bytes_;
    }

    [[nodiscard]] ipc::IdrRecoveryCoordinator::RequestSnapshot idr_snapshot() const {
        std::lock_guard lock(mutex_);
        return idr_coordinator_.snapshot();
    }

    [[nodiscard]] core::Result<bool> request_idr_external(std::string_view reason) {
        std::unique_lock lock(mutex_);
        if (idr_dispatching_.load(std::memory_order_acquire) ||
            !has_idr_request_hook_locked() || !projecting_locked()) {
            return false;
        }
        const auto now = now_locked();
        begin_idr_wait_locked(now, reason);
        return dispatch_idr_request_locked(lock, now, reason);
    }

    [[nodiscard]] core::Result<void> service_pending() {
        std::unique_lock lock(mutex_);
        if (idr_dispatching_.load(std::memory_order_acquire)) { return {}; }
        if (auto released = release_due_locked(lock, now_locked()); !released) {
            return released.error();
        }
        if (idr_dispatching_.load(std::memory_order_acquire) ||
            pending_queue_.empty() || !producer_.has_value()) {
            return {};
        }
        const auto drained = drain_pending_locked(lock);
        if (!drained) {
            return drained.error();
        }
        return {};
    }

    void reset() noexcept {
        std::lock_guard lock(mutex_);
        clear_delayed_locked("reset");
        clear_pending_locked(/*abandon_partial=*/true, "reset");
        idr_coordinator_.reset();
        had_backpressure_ = false;
        replaced_consumer_ = false;
        armed_late_consumer_ = false;
        profile_.reset();
        session_.reset();
        generation_.reset();
        av1_reduced_ = false;
    }

    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override {
        std::unique_lock lock(mutex_);
        if (delay_.count() == 0 && delayed_.empty()) { return deliver_locked(lock, message); }
        const auto now = now_locked();
        if (auto released = release_due_locked(lock, now); !released) { return released.error(); }
        // Bounded hold: past it the oldest unit leaves early rather than grow.
        while (!delayed_.empty() && (delayed_.size() >= kMaxDelayedUnits ||
                                     delayed_bytes_ + message.payload.size() > kMaxDelayedBytes)) {
            if (auto released = release_front_locked(lock); !released) { return released.error(); }
        }
        if (delay_.count() == 0) {
            while (!delayed_.empty()) {
                if (auto released = release_front_locked(lock); !released) { return released.error(); }
            }
            return deliver_locked(lock, message);
        }
        delayed_bytes_ += message.payload.size();
        delayed_.push_back(DelayedMessage{message.header,
            {message.payload.begin(), message.payload.end()}, now + delay_});
        return {};
    }

private:
    [[nodiscard]] core::Result<void> deliver_locked(std::unique_lock<std::mutex>& lock,
                                                    const protocol::MessageView& message) {
        if (!producer_.has_value()) {
            ++dropped_before_consumer_;
            record_drop("no_consumer", frame_cadence(message.header.video_profile));
            return {};
        }
        if (message.header.video_profile != profile_ || message.header.video_session != session_ || message.header.video_generation != generation_) {
            if (!pending_queue_.empty()) {
                // Stream epoch/profile changed while AUs were held: drop held frames,
                // record lifecycle drops, and force waiting_idr on the retained producer
                // so non-IDR frames from the new epoch cannot splice in.
                clear_pending_locked(/*abandon_partial=*/true, "stream_reset");
            }
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
        // Flush any held AUs before admitting the new AU so access-unit order is
        // strictly preserved. If draining overflows, waiting_idr is entered and
        // the incoming AU obeys waiting_idr below.
        if (!pending_queue_.empty()) {
            const auto drained = drain_pending_locked(lock);
            if (!drained) {
                return drained.error();
            }
            if (!producer_.has_value()) {
                ++dropped_before_consumer_;
                record_drop("no_consumer", frame_cadence(message.header.video_profile));
                return {};
            }
        }

        std::vector<std::uint8_t> payload;
        payload.reserve(message.payload.size());
        for (const std::byte byte : message.payload) {
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        }
        const bool is_idr = is_idr_au(
            payload, profile_ ? profile_->codec : protocol::VideoCodec::h264, &av1_reduced_);
        const std::uint64_t frame_id = ++sequence_;
        const std::uint64_t timestamp_ns =
            static_cast<std::uint64_t>(message.header.timestamp.count);
        const auto now = now_locked();
        const auto effective_profile =
            message.header.video_profile ? message.header.video_profile : profile_;
        const auto budget = ipc::recommended_backpressure_budget(effective_profile);

        if (!pending_queue_.empty()) {
            const auto age = now - pending_queue_.front().enqueued_at;
            if (age >= budget.max_age || pending_bytes_ + payload.size() > budget.max_bytes) {
                return overflow_backpressure_locked(lock, /*extra_drops=*/1);
            }
            enqueue_pending_locked(frame_id, timestamp_ns, is_idr, std::move(payload),
                                   message.header.video_profile, now, budget);
            return {};
        }

        const auto sent = producer_->try_send_access_unit(ipc::VideoSendRequest{
            frame_id, timestamp_ns, is_idr, payload, message.header.video_profile});
        if (!sent) {
            if (handle_transport_error_locked(sent.error(), /*extra_drops=*/1)) {
                return {};
            }
            return sent.error();
        }
        switch (sent.value()) {
        case ipc::TrySendStatus::would_block:
            if (payload.size() > budget.max_bytes) {
                return overflow_backpressure_locked(lock, /*extra_drops=*/1);
            }
            enqueue_pending_locked(frame_id, timestamp_ns, is_idr, std::move(payload),
                                   message.header.video_profile, now, budget);
            return {};
        case ipc::TrySendStatus::dropped_waiting_idr: {
            record_drop("waiting_idr", frame_cadence(profile_));
            const bool past_startup_grace =
                producer_installed_at_.has_value() &&
                (now - *producer_installed_at_) >= ipc::kIdrStartupGrace;
            if (had_backpressure_ || units_ > 0 || replaced_consumer_ ||
                armed_late_consumer_ || past_startup_grace) {
                if (auto idr = maybe_request_idr_locked(lock, "waiting_idr"); !idr) {
                    return idr.error();
                }
            }
            return {};
        }
        case ipc::TrySendStatus::sent:
            note_au_sent_locked(is_idr, now);
            break;
        }
        return {};
    }

public:
    void install(ipc::VideoProducer producer) {
        std::lock_guard lock(mutex_);
        clear_pending_locked(/*abandon_partial=*/false, "consumer_replaced");
        // Endpoint events fire once per access unit (audit L13), so they are media
        // telemetry: rate-limited and never waiting for the sink. The producer only
        // observes send_started; receive_completed is a consumer-side kind.
        producer.set_observer([this](const ipc::VideoEndpoint& event) {
            auto line = ipc::video_endpoint_json(event);
            // Both endpoint kinds are per access unit: never the lifecycle sink.
            media(event.kind == ipc::VideoEndpointKind::send_started ? "video_ipc_send"
                                                                     : "video_ipc_receive",
                  line);
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
        had_backpressure_ = false;
        producer_installed_at_ = now_locked();
        producer_.emplace(std::move(producer));
        if (profile_) {
            configure_buffer_locked(*profile_);
        }
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        clear_delayed_locked("closed");
        clear_pending_locked(/*abandon_partial=*/false, "closed");
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
    struct DelayedMessage final {
        protocol::MessageHeader header{};
        std::vector<std::byte> payload{};
        std::chrono::steady_clock::time_point due{};
    };
    // 180 ms at 60 fps is 11 units; the byte bound covers IDR-heavy bursts.
    static constexpr std::size_t kMaxDelayedUnits = 64;
    static constexpr std::size_t kMaxDelayedBytes = std::size_t{32} * 1024 * 1024;

    [[nodiscard]] core::Result<void> release_front_locked(std::unique_lock<std::mutex>& lock) {
        auto item = std::move(delayed_.front());
        delayed_.pop_front();
        delayed_bytes_ -= item.payload.size();
        return deliver_locked(lock, protocol::MessageView{item.header, item.payload});
    }

    [[nodiscard]] core::Result<void> release_due_locked(std::unique_lock<std::mutex>& lock,
                                                        std::chrono::steady_clock::time_point now) {
        while (!delayed_.empty() && delayed_.front().due <= now &&
               !idr_dispatching_.load(std::memory_order_acquire)) {
            if (auto released = release_front_locked(lock); !released) { return released.error(); }
        }
        return {};
    }

    void clear_delayed_locked(const char* drop_reason) noexcept {
        const auto cadence = frame_cadence(profile_);
        for (std::size_t i = 0; i < delayed_.size(); ++i) { record_drop(drop_reason, cadence); }
        delayed_.clear();
        delayed_bytes_ = 0;
    }

    struct PendingAccessUnit final {
        std::uint64_t sequence{};
        std::uint64_t timestamp_ns{};
        bool idr{false};
        std::vector<std::uint8_t> payload{};
        std::optional<protocol::VideoProfile> profile{};
        std::chrono::steady_clock::time_point enqueued_at{};
    };

    enum class DrainOutcome : std::uint8_t {
        empty,
        still_held,
        overflowed,
        disconnected,
    };

    void reset_pending_state_locked() noexcept {
        pending_queue_.clear();
        pending_bytes_ = 0;
        hold_episode_started_at_ = {};
        hold_peak_bytes_ = 0;
        hold_episode_units_ = 0;
    }

    void clear_pending_locked(bool abandon_partial, const char* drop_reason) noexcept {
        const auto dropped_units = pending_queue_.size();
        const auto cadence = frame_cadence(profile_);
        reset_pending_state_locked();
        if (dropped_units > 0 && drop_reason != nullptr) {
            for (std::size_t i = 0; i < dropped_units; ++i) {
                record_drop(drop_reason, cadence);
            }
        }
        if (abandon_partial && dropped_units > 0 && producer_.has_value()) {
            producer_->abandon_partial_send();
        }
    }

    void enqueue_pending_locked(std::uint64_t sequence,
                                std::uint64_t timestamp_ns,
                                bool is_idr,
                                std::vector<std::uint8_t> payload,
                                const std::optional<protocol::VideoProfile>& profile,
                                std::chrono::steady_clock::time_point now,
                                const ipc::VideoBackpressureBudget& budget) {
        const bool starting_hold = pending_queue_.empty();
        pending_bytes_ += payload.size();
        pending_queue_.push_back(PendingAccessUnit{
            sequence, timestamp_ns, is_idr, std::move(payload), profile, now});
        ++hold_episode_units_;
        hold_peak_bytes_ = std::max(hold_peak_bytes_, pending_bytes_);
        if (starting_hold) {
            hold_episode_started_at_ = now;
            media("video_backpressure_held",
                  ipc::video_backpressure_held_json(
                      pending_queue_.size(), pending_bytes_, 0, budget.max_bytes,
                      budget.max_age.count()));
        }
    }

    void note_au_sent_locked(bool is_idr, std::chrono::steady_clock::time_point now) {
        had_backpressure_ = false;
        replaced_consumer_ = false;
        armed_late_consumer_ = false;
        if (is_idr && idr_coordinator_.waiting()) {
            const auto recovered = idr_coordinator_.on_idr_delivered(now);
            if (recovered.recovered) {
                log_(ipc::idr_recovered_json("producer", recovered.latency_ms,
                                             recovered.episode_requests,
                                             recovered.total_requests));
            }
        }
        ++units_;
        if (!has_sent_report_ || units_ - last_sent_report_unit_ >= frame_cadence(profile_)) {
            has_sent_report_ = true;
            last_sent_report_unit_ = units_;
            media("video_sent", "{\"event\":\"video_sent\",\"units\":" + std::to_string(units_) + "}");
            emit_drops(false);
        }
    }

    [[nodiscard]] bool handle_transport_error_locked(const Error& err, std::size_t extra_drops) {
        // Error erases errno; confirm peer hangup rather than hiding every I/O failure.
        if (err.code() != ErrorCode::transport_io || !producer_.has_value()) {
            return false;
        }
        pollfd peer{producer_->native_handle(), 0, 0};
        int ready;
        do {
            ready = ::poll(&peer, 1, 0);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || (peer.revents & POLLHUP) == 0) {
            return false;
        }
        const std::size_t lost_units = pending_queue_.size() + extra_drops;
        reset_pending_state_locked();
        producer_->close();
        producer_.reset();
        idr_coordinator_.reset();
        had_backpressure_ = false;
        armed_late_consumer_ = false;
        producer_installed_at_.reset();
        const auto cadence = frame_cadence(profile_);
        for (std::size_t i = 0; i < lost_units; ++i) {
            record_drop("consumer_disconnected", cadence);
        }
        return true;
    }

    [[nodiscard]] core::Result<void> overflow_backpressure_locked(
        std::unique_lock<std::mutex>& lock, std::size_t extra_drops) {
        const std::size_t dropped_units = pending_queue_.size() + extra_drops;
        reset_pending_state_locked();
        if (producer_.has_value()) {
            producer_->enter_waiting_idr_on_backpressure();
        }
        had_backpressure_ = true;
        const auto cadence = frame_cadence(profile_);
        for (std::size_t i = 0; i < dropped_units; ++i) {
            record_drop("backpressure", cadence);
        }
        if (auto idr = maybe_request_idr_locked(lock, "backpressure"); !idr) {
            return idr.error();
        }
        return {};
    }

    [[nodiscard]] core::Result<DrainOutcome> drain_pending_locked(
        std::unique_lock<std::mutex>& lock) {
        while (!pending_queue_.empty() && producer_.has_value()) {
            const auto now = now_locked();
            const auto& front = pending_queue_.front();
            const auto budget =
                ipc::recommended_backpressure_budget(front.profile ? front.profile : profile_);
            if ((now - front.enqueued_at) >= budget.max_age) {
                if (auto overflow = overflow_backpressure_locked(lock, /*extra_drops=*/0);
                    !overflow) {
                    return overflow.error();
                }
                return DrainOutcome::overflowed;
            }
            const auto sent = producer_->try_send_access_unit(ipc::VideoSendRequest{
                front.sequence, front.timestamp_ns, front.idr, front.payload, front.profile});
            if (!sent) {
                if (handle_transport_error_locked(sent.error(), /*extra_drops=*/0)) {
                    return DrainOutcome::disconnected;
                }
                return sent.error();
            }
            switch (sent.value()) {
            case ipc::TrySendStatus::would_block:
                return DrainOutcome::still_held;
            case ipc::TrySendStatus::dropped_waiting_idr:
                pending_bytes_ -= front.payload.size();
                pending_queue_.pop_front();
                record_drop("waiting_idr", frame_cadence(profile_));
                if (pending_queue_.empty()) {
                    hold_episode_units_ = 0;
                    hold_peak_bytes_ = 0;
                    hold_episode_started_at_ = {};
                }
                break;
            case ipc::TrySendStatus::sent: {
                const bool was_idr = front.idr;
                pending_bytes_ -= front.payload.size();
                pending_queue_.pop_front();
                note_au_sent_locked(was_idr, now);
                if (pending_queue_.empty()) {
                    const auto held_units = hold_episode_units_;
                    const auto held_bytes = hold_peak_bytes_;
                    const auto age_ms = std::max<std::int64_t>(
                        0, std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - hold_episode_started_at_).count());
                    hold_episode_units_ = 0;
                    hold_peak_bytes_ = 0;
                    hold_episode_started_at_ = {};
                    media("video_backpressure_released",
                          ipc::video_backpressure_released_json(
                              held_units, held_bytes, age_ms));
                }
                break;
            }
            }
        }
        return DrainOutcome::empty;
    }

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
        const auto observations = units_ + dropped_;
        // The final cumulative record goes to the lifecycle sink once per count,
        // even when a periodic record with the same count went to the (lossy)
        // media sink and may have been dropped (#90 review P2).
        const auto exported = final ? final_observations_ : exported_observations_;
        if (observations == 0 || exported == observations) {
            return;
        }
        exported_observations_ = observations;
        if (final) { final_observations_ = observations; }
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

    [[nodiscard]] core::Result<void> maybe_request_idr_locked(
        std::unique_lock<std::mutex>& lock, std::string_view reason) {
        if (idr_dispatching_.load(std::memory_order_acquire) || !has_idr_request_hook_locked()) {
            return {};
        }
        const auto now = now_locked();
        begin_idr_wait_locked(now, reason);
        const auto dispatched = dispatch_idr_request_locked(lock, now, reason);
        if (!dispatched && dispatched.error().code() == ErrorCode::channel_focus_restore_failed) {
            return dispatched.error();
        }
        return {};
    }

    // The wait transition is logged before any request (#90 review 5), so a
    // producer stall where every request fails still opens an episode.
    void begin_idr_wait_locked(std::chrono::steady_clock::time_point now, std::string_view reason) {
        if (!idr_coordinator_.waiting()) {
            log_(ipc::idr_wait_started_json("producer", reason));
        }
        idr_coordinator_.begin_wait(now);
    }

    [[nodiscard]] core::Result<bool> dispatch_idr_request_locked(
        std::unique_lock<std::mutex>& lock,
        std::chrono::steady_clock::time_point now,
        std::string_view reason) {
        const auto step = idr_coordinator_.try_request_step(now, projecting_locked());
        if (step == ipc::IdrRecoveryStep::none) {
            return false;
        }
        const bool use_transition = (step == ipc::IdrRecoveryStep::focus_transition);
        const auto hook_with_mode = idr_hooks_.request_idr_with_mode;
        const auto hook = idr_hooks_.request_idr;
        const auto ep_requests = idr_coordinator_.episode_requests();
        const auto tot_requests = idr_coordinator_.total_requests();
        idr_dispatching_.store(true, std::memory_order_release);
        lock.unlock();
        const auto sent = hook_with_mode ? hook_with_mode(use_transition) : hook();
        lock.lock();
        idr_dispatching_.store(false, std::memory_order_release);
        if (!sent) {
            if (sent.error().code() == ErrorCode::channel_focus_restore_failed) {
                // Phone may have received the preceding `native` focus notification;
                // preserve cooldown, request counts, and escalated_ state so native
                // is never replayed, and propagate the terminal failure.
                log_(ipc::idr_transition_failed_json("producer", "restore_focus", reason,
                                                     ep_requests, tot_requests,
                                                     to_string(sent.error().code())));
                return sent.error();
            }
            idr_coordinator_.note_request_failed(now);
            return sent.error();
        }
        log_(ipc::idr_requested_json("producer", reason));
        if (use_transition) {
            log_(ipc::idr_escalated_json("producer", ipc::idr_recovery_step_name(step), reason,
                                         ep_requests, tot_requests));
        }
        return true;
    }

    std::function<void(const std::string&)> log_;
    MediaLog media_log_;
    IdrRecoveryHooks idr_hooks_{};
    ipc::IdrRecoveryCoordinator idr_coordinator_{};
    mutable std::mutex mutex_{};
    std::atomic<bool> idr_dispatching_{false};
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
    std::deque<PendingAccessUnit> pending_queue_{};
    std::chrono::nanoseconds delay_{0};
    std::deque<DelayedMessage> delayed_{};
    std::size_t delayed_bytes_{0};
    std::size_t pending_bytes_{0};
    std::chrono::steady_clock::time_point hold_episode_started_at_{};
    std::size_t hold_peak_bytes_{0};
    std::uint64_t hold_episode_units_{0};
    std::uint64_t sequence_{};
    std::uint64_t units_{};
    std::uint64_t last_sent_report_unit_{};
    bool has_sent_report_{};
    std::uint64_t dropped_{};
    std::uint64_t last_drop_report_count_{};
    bool has_drop_report_{};
    std::uint64_t exported_observations_{};
    std::uint64_t final_observations_{};
};

} // namespace aa::wired_receiver
