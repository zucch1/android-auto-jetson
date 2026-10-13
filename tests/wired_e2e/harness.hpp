// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared test-only doubles and frame builders for the task-30 wired
// end-to-end suite. All payloads are fixed synthetic bytes; no live capture.

#include <aa/app/WiredSession.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/Framing.hpp>

// Private (non-installed) router seam: the test wires the real task-25 router
// around a recording sink instead of the PipeWire default sink. Included at
// file scope (never inside the test namespace) so its nested namespaces stay
// top-level.
#ifdef AA_WIRED_HAS_AUDIO_ROUTER
#include "../../src/audio/AudioSinkFactory.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wiredtest {

// Scripted transport double: the fake wired link. Serves queued inbound frames
// and records every outbound frame; open()/close() are counted.
class ScriptedTransport final : public aa::transport::Transport {
  public:
    [[nodiscard]] aa::transport::Kind kind() const noexcept override {
        return aa::transport::Kind::usb_aoa;
    }

    aa::core::Result<void> open(aa::core::CancellationToken cancellation) override {
        token_ = cancellation;
        open_ = true;
        ++open_calls_;
        return {};
    }

    aa::core::Result<void> send(std::span<const std::byte> frame) override {
        if (!open_) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        sent_.emplace_back(frame.begin(), frame.end());
        return {};
    }

    aa::core::Result<std::vector<std::byte>> receive() override {
        if (!open_) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        if (inbound_.empty()) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        auto frame = std::move(inbound_.front());
        inbound_.erase(inbound_.begin());
        return frame;
    }

    void close() noexcept override {
        open_ = false;
        ++close_calls_;
    }

    void push_inbound(std::vector<std::byte> frame) { inbound_.push_back(std::move(frame)); }

    [[nodiscard]] std::size_t sent_count() const noexcept { return sent_.size(); }
    [[nodiscard]] const std::vector<std::byte> &last_sent() const noexcept { return sent_.back(); }
    [[nodiscard]] int open_calls() const noexcept { return open_calls_; }
    [[nodiscard]] int close_calls() const noexcept { return close_calls_; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

  private:
    std::vector<std::vector<std::byte>> inbound_{};
    std::vector<std::vector<std::byte>> sent_{};
    std::optional<aa::core::CancellationToken> token_{};
    bool open_{false};
    int open_calls_{0};
    int close_calls_{0};
};

// Capturing diagnostics sink (the task-19 redaction boundary output).
class CaptureSink final : public aa::diagnostics::DiagnosticSink {
  public:
    void emit_json(std::string_view redacted_json) override { jsons.emplace_back(redacted_json); }

    [[nodiscard]] bool contains(std::string_view needle) const {
        for (const auto &json : jsons) {
            if (json.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> jsons{};
};

// Records every message the session routes to a channel sink.
class RecordingSink final : public aa::protocol::MessageSink {
  public:
    aa::core::Result<void> on_message(const aa::protocol::MessageView &message) override {
        headers.push_back(message.header);
        payloads.emplace_back(message.payload.begin(), message.payload.end());
        return {};
    }

    std::vector<aa::protocol::MessageHeader> headers{};
    std::vector<std::vector<std::byte>> payloads{};
};

// ---- frame builders (reuse the project wire codec) ----

inline std::vector<std::byte> control_frame(std::vector<std::byte> payload) {
    auto frames = aa::transport::encode_message(
        aa::transport::Message{std::uint8_t{0}, aa::transport::MessageKind::control,
                               std::move(payload)},
        aa::transport::Encryption::plain, nullptr);
    return frames.value().front();
}

inline std::vector<std::byte> discovery_request_frame() {
    return control_frame({std::byte{0x00}, std::byte{0x05}});
}

inline std::vector<std::byte> open_request_frame(std::int32_t service) {
    return control_frame(
        aa::protocol::encode(aa::protocol::ChannelOpenRequest{1, aa::protocol::ServiceKey{service}})
            .value());
}

inline std::vector<std::byte> service_frame(std::uint8_t channel, std::vector<std::byte> payload) {
    auto frames = aa::transport::encode_message(
        aa::transport::Message{channel, aa::transport::MessageKind::specific, std::move(payload)},
        aa::transport::Encryption::plain, nullptr);
    return frames.value().front();
}

// ---- service descriptors (video / media audio / input) ----

inline std::vector<aa::protocol::ServiceDescriptor> golden_services() {
    return {
        {aa::protocol::ServiceKey{99}, aa::protocol::ChannelRole::video,
         aa::protocol::VideoConfiguration{{{1280, 720, 30}, {800, 480, 30}}}},
        {aa::protocol::ServiceKey{100}, aa::protocol::ChannelRole::media_audio,
         aa::protocol::AudioConfiguration{{{48000, 16, 2}}}},
        {aa::protocol::ServiceKey{101}, aa::protocol::ChannelRole::input,
         aa::protocol::ButtonConfiguration{{66}}},
    };
}

// A fixed synthetic IDR-shaped video access unit (Annex-B start code + NAL).
inline const std::vector<std::byte> kVideoIdr = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                                 std::byte{0x01}, std::byte{0x65}, std::byte{0x88}};

// 960 stereo 16-bit frames of constant tone (3840 bytes), the router's PCM
// unit for one media_audio submit.
inline std::vector<std::byte> pcm_unit() {
    return std::vector<std::byte>(960UL * 2UL * sizeof(std::int16_t), std::byte{0x40});
}

// ---- real task-25 audio router wiring (when the archive is present) ----
#ifdef AA_WIRED_HAS_AUDIO_ROUTER

// Out-of-band observation block: survives the router that owns the sink.
struct AudioObs final {
    std::uint64_t writes{};
    std::size_t last_bytes{};
};

class RecordingObservedSink final : public aa::audio::detail::ObservedAudioSink {
  public:
    explicit RecordingObservedSink(std::shared_ptr<AudioObs> obs) : obs_(std::move(obs)) {}

    aa::core::Result<void> write(const aa::audio::MixedChunk &chunk) override {
        ++obs_->writes;
        obs_->last_bytes = chunk.pcm.size();
        return {};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    [[nodiscard]] aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    [[nodiscard]] aa::audio::RenderBarrierStats render_stats() const override { return {}; }

  private:
    std::shared_ptr<AudioObs> obs_;
};

// Forwards each routed media_audio message into the real router and pumps it.
class RouterAudioSink final : public aa::protocol::MessageSink {
  public:
    explicit RouterAudioSink(aa::audio::AudioRouter &router) : router_(router) {}

    aa::core::Result<void> on_message(const aa::protocol::MessageView &message) override {
        if (auto submitted = router_.submit(message); !submitted) {
            return submitted.error();
        }
        return router_.pump();
    }

  private:
    aa::audio::AudioRouter &router_;
};

inline aa::audio::RouterConfig pcm_router_config() {
    aa::audio::RouterConfig config;
    config.output_format = aa::audio::AudioFormat{48000, 2, 16};
    for (auto &role : config.roles) {
        role.codec = aa::audio::Codec::pcm;
        role.input_format = aa::audio::AudioFormat{48000, 2, 16};
    }
    return config;
}
#endif

} // namespace wiredtest
