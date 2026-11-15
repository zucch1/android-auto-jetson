// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ServiceDescription.hpp"
#include <aa/app/WirelessLink.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/channels/InputSink.hpp>
#include <aa/channels/MicrophoneSink.hpp>
#include <aa/channels/SensorSink.hpp>
#include <aa/config/VideoProfiles.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include "../wired-receiver/VideoOutlet.hpp"
#include <cstdio>
#include <utility>

namespace aa::wireless_receiver {
inline void log_line(const std::string& line) {
    std::fputs(line.c_str(), stdout); std::fputc('\n', stdout); std::fflush(stdout);
}
inline void report(const char* operation, ErrorCode code) {
    std::fprintf(stderr, "aa-wireless-receiver: %s: %.*s\n", operation,
        static_cast<int>(to_string(code).size()), to_string(code).data());
}
class DiagnosticOutput final : public diagnostics::DiagnosticSink {
public:
    explicit DiagnosticOutput(bool enabled) : enabled_(enabled) {}
    void emit_json(std::string_view json) override {
        if (enabled_) log_line(std::string{json});
    }
private:
    bool enabled_;
};
class AudioSink final : public protocol::MessageSink {
public:
    explicit AudioSink(audio::AudioRouter& router) : router_(router) {}
    core::Result<void> on_message(const protocol::MessageView& message) override {
        if (auto result = router_.submit(message); !result) return result.error();
        const auto pumped = router_.pump();
        return pumped ? core::Result<void>{} : core::Result<void>{pumped.error()};
    }
private:
    audio::AudioRouter& router_;
};
inline audio::RouterConfig audio_config() {
    audio::RouterConfig config;
    config.output_format = {48000, 2, 16};
    for (auto& role : config.roles) {
        role.codec = audio::Codec::pcm;
        role.input_format = {16000, 1, 16};
    }
    config.roles[audio::role_index(audio::Role::media)].input_format = {48000, 2, 16};
    return config;
}

// The media endpoints outlive every per-link session; callbacks resolve the
// current session instead of capturing an address that recovery can invalidate.
struct Media final {
    wired_receiver::VideoOutlet video{log_line};
    AudioSink audio;
    channels::InputSink input;
    input::CapabilityDescriptor capabilities{input::CapabilityDescriptor::bench_defaults()};
    std::function<core::Result<void>(protocol::ChannelRole, std::vector<std::byte>)> send;
    channels::SensorSink sensors{capabilities, [this](std::vector<std::byte> payload) {
        return send(protocol::ChannelRole::sensors, std::move(payload));
    }};
    channels::MicrophoneSink microphone{[this](std::vector<std::byte> payload) {
        return send(protocol::ChannelRole::microphone, std::move(payload));
    }};
    explicit Media(audio::AudioRouter& router) : audio(router) {}
    app::WirelessLink::Config config(
        std::vector<protocol::VideoProfile> profiles = aa::config::default_video_profiles()) {
        using protocol::ChannelRole;
        app::WirelessLink::Config config;
        config.session.input_enabled = true;
        config.session.input = capabilities;
        config.services = service_descriptors(capabilities, std::move(profiles));
        config.sinks = {{ChannelRole::input, &input}, {ChannelRole::sensors, &sensors},
            {ChannelRole::microphone, &microphone}};
        return config;
    }
    core::Result<void> bind(app::WiredSession& session) {
        using protocol::ChannelRole;
        if (auto bound = session.register_av_sink(ChannelRole::video, video); !bound) return bound.error();
        for (const auto role : {ChannelRole::media_audio, ChannelRole::guidance_audio, ChannelRole::system_audio}) {
            if (auto bound = session.register_av_sink(role, audio); !bound) return bound.error();
        }
        return {};
    }
};
} // namespace aa::wireless_receiver
