// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test-only fakes and the golden synthetic scenario for the task-20 replay
// suite. All payloads are fixed synthetic bytes (no live capture); expected
// states and outcome literals are hand-written constants, never derived from
// a run.

#include <aa/audio/Audio.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/replay/Recorder.hpp>
#include <aa/transport/Framing.hpp>
#include <aa/trust/Trust.hpp>

#include "tamper.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace replaytest {
namespace proto = aa::protocol;
namespace replay = aa::replay;

inline const aa::trust::PhoneIdentity kApproved{"phone-approved"};
inline const aa::trust::PhoneIdentity kUnknown{"phone-unknown"};

class MapTrustStore final : public aa::trust::TrustStore {
public:
    MapTrustStore() { store_[kApproved.key] = aa::trust::Decision::approved; }
    [[nodiscard]] aa::trust::Decision lookup(const aa::trust::PhoneIdentity& identity)
        const override {
        const auto found = store_.find(identity.key);
        return found == store_.end() ? aa::trust::Decision::unknown : found->second;
    }
    aa::core::Result<void> approve_once(const aa::trust::PhoneIdentity& identity) override {
        store_[identity.key] = aa::trust::Decision::approved;
        return {};
    }
    aa::core::Result<void> forget(const aa::trust::PhoneIdentity& identity) override {
        store_.erase(identity.key);
        return {};
    }

private:
    std::map<std::string, aa::trust::Decision> store_{};
};

class RecordingSink final : public proto::MessageSink {
public:
    aa::core::Result<void> on_message(const proto::MessageView& message) override {
        headers.push_back(message.header);
        payloads.emplace_back(message.payload.begin(), message.payload.end());
        return {};
    }
    std::vector<proto::MessageHeader> headers{};
    std::vector<std::vector<std::byte>> payloads{};
};

class AudioCapture final : public aa::audio::AudioSink {
public:
    struct Chunk final {
        aa::audio::Role role{};
        aa::audio::AudioFormat format{};
        aa::core::Nanoseconds timestamp{};
        std::vector<std::byte> pcm{};
    };

    aa::core::Result<void> write(const aa::audio::PcmChunk& chunk) override {
        chunks.push_back(Chunk{chunk.role, chunk.format, chunk.timestamp,
                               std::vector<std::byte>(chunk.pcm.begin(), chunk.pcm.end())});
        return {};
    }
    void flush() noexcept override { ++flushes; }

    std::vector<Chunk> chunks{};
    int flushes{};
};

inline std::vector<std::byte> make_bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<std::byte>(value & 0xFF));
    }
    return out;
}

inline std::vector<std::byte> control_frame(std::vector<std::byte> payload) {
    auto frames = aa::transport::encode_message(
        aa::transport::Message{std::uint8_t{0}, aa::transport::MessageKind::control,
                               std::move(payload)},
        aa::transport::Encryption::plain, nullptr);
    return frames.value().front();
}

inline std::vector<std::byte> discovery_request_frame() {
    return control_frame(make_bytes({0x00, 0x05}));
}

inline std::vector<std::byte> discovery_response_frame(
    const std::vector<replay::ServiceEntry>& services) {
    proto::ServiceDiscoveryResponse response;
    for (const auto& service : services) {
        response.services.push_back({service.id, service.role, service.configuration});
    }
    return control_frame(proto::encode(response).value());
}

inline std::vector<std::byte> open_request_frame(std::int32_t service) {
    return control_frame(
        proto::encode(proto::ChannelOpenRequest{1, proto::ServiceKey{service}}).value());
}

inline std::vector<std::byte> open_response_frame() {
    return control_frame(
        proto::encode(proto::ChannelOpenResponse{proto::ChannelOpenStatus::success}).value());
}

inline std::vector<replay::ServiceEntry> golden_services() {
    return {
        replay::ServiceEntry{
            proto::ServiceKey{99}, proto::ChannelRole::video,
            proto::VideoConfiguration{{{1280, 720, 30}, {800, 480, 30}}}},
        replay::ServiceEntry{
            proto::ServiceKey{100}, proto::ChannelRole::media_audio,
            proto::AudioConfiguration{{{48000, 16, 2}}}},
        replay::ServiceEntry{proto::ServiceKey{101}, proto::ChannelRole::input,
                             proto::ButtonConfiguration{{85}}},
    };
}

inline const std::vector<std::byte> kVideoOne =
    make_bytes({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00});
inline const std::vector<std::byte> kVideoTwo =
    make_bytes({0x00, 0x00, 0x00, 0x01, 0x41, 0x9a});
inline const std::vector<std::byte> kInputOne = make_bytes({0x01, 0x55});
inline const std::vector<std::byte> kPcmOne =
    make_bytes({0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80});

inline constexpr std::string_view kGoldenFixtureSha256 =
    "1b7f939d4174424212a3ee0da1fca74b678980bdba2523f5560e79ec08683e48";

// Hand-written expected output trace for the golden scenario.
inline replay::Expectation golden_expectation() {
    replay::Expectation expect;
    expect.states = {"disconnected", "discovering", "connecting", "negotiating", "active"};
    expect.deliveries = {
        {proto::ChannelRole::video, 1, aa::core::Nanoseconds{5'000'000}, kVideoOne},
        {proto::ChannelRole::input, 1, aa::core::Nanoseconds{6'000'000}, kInputOne},
        {proto::ChannelRole::video, 2, aa::core::Nanoseconds{12'000'000}, kVideoTwo},
    };
    expect.audio = {{aa::audio::Role::media, aa::audio::AudioFormat{48000, 2, 16},
                     aa::core::Nanoseconds{7'000'000}, kPcmOne}};
    expect.sends = 5;
    return expect;
}

// Golden session script: discovery -> connect -> negotiate -> control opens ->
// video/input/audio -> active -> teardown reset -> reopened routing.
inline void fill_golden(replay::Recorder& recorder) {
    const auto services = golden_services();
    (void)recorder.set_services(services);
    (void)recorder.set_metadata(aa::diagnostics::EventField::text("state", "active"));
    (void)recorder.set_metadata(aa::diagnostics::EventField::text("transport", "usb"));
    (void)recorder.set_metadata(aa::diagnostics::EventField::text("codec", "h264"));
    (void)recorder.set_metadata(
        aa::diagnostics::EventField::identifier("phone_id", "AA:BB:CC:DD:EE:FF"));
    (void)recorder.set_metadata(
        aa::diagnostics::EventField::credential("token", "hunter2-secret"));
    const auto discovery = discovery_request_frame();
    const auto discovery_out = discovery_response_frame(services);
    const auto open99 = open_request_frame(99);
    const auto open101 = open_request_frame(101);
    const auto open_out = open_response_frame();
    using aa::core::Nanoseconds;
    using replay::Direction;
    using replay::SessionOp;
    using replay::SessionRecord;
    (void)recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery});
    (void)recorder.record_session(
        Nanoseconds{1'000'000},
        SessionRecord{SessionOp::phone_discovered, {}, kApproved.key});
    (void)recorder.record_session(
        Nanoseconds{2'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::transport_ready});
    (void)recorder.record_transport(Nanoseconds{3'000'000}, Direction::inbound, discovery);
    (void)recorder.record_transport(Nanoseconds{3'500'000}, Direction::outbound, discovery_out);
    (void)recorder.record_transport(Nanoseconds{4'000'000}, Direction::inbound, open99);
    (void)recorder.record_transport(Nanoseconds{4'500'000}, Direction::outbound, open_out);
    (void)recorder.record_transport(Nanoseconds{4'600'000}, Direction::inbound, open101);
    (void)recorder.record_transport(Nanoseconds{4'700'000}, Direction::outbound, open_out);
    (void)recorder.record_video(
        Nanoseconds{5'000'000},
        replay::VideoRecord{1, 1, Nanoseconds{5'000'000}, true, kVideoOne});
    (void)recorder.record_input(
        Nanoseconds{6'000'000}, replay::InputRecord{1, Nanoseconds{6'000'000}, kInputOne});
    (void)recorder.record_audio(
        Nanoseconds{7'000'000},
        replay::AudioRecord{aa::audio::Role::media, aa::audio::AudioFormat{48000, 2, 16},
                            Nanoseconds{7'000'000}, kPcmOne});
    (void)recorder.record_session(
        Nanoseconds{8'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::negotiation_succeeded});
    (void)recorder.record_session(Nanoseconds{9'000'000},
                                  SessionRecord{SessionOp::reset_channels});
    (void)recorder.record_transport(Nanoseconds{10'000'000}, Direction::inbound, discovery);
    (void)recorder.record_transport(Nanoseconds{10'500'000}, Direction::outbound, discovery_out);
    (void)recorder.record_transport(Nanoseconds{11'000'000}, Direction::inbound, open99);
    (void)recorder.record_transport(Nanoseconds{11'500'000}, Direction::outbound, open_out);
    (void)recorder.record_video(
        Nanoseconds{12'000'000},
        replay::VideoRecord{2, 2, Nanoseconds{12'000'000}, false, kVideoTwo});
    (void)recorder.set_expectation(golden_expectation());
}

} // namespace replaytest
