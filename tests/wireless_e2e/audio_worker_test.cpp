// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/audio/AudioSinkFactory.hpp"
#include "../../tools/wireless-receiver/Media.hpp"
#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"
#include "../wired_e2e/harness.hpp"

#include <aa/app/AvAudioRegistration.hpp>
#include <aa/app/WiredSession.hpp>
#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/audio/AudioTelemetry.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/diagnostics/StdoutLog.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/transport/Framing.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace app = aa::app;
namespace audio = aa::audio;
namespace diag = aa::diagnostics;
namespace ipc = aa::ipc;
namespace proto = aa::protocol;
namespace ss = aa::session;
namespace tr = aa::transport;
namespace wr = aa::wireless_receiver;

std::vector<std::byte> av_data_message(std::uint64_t peer_timestamp,
                                       const std::vector<std::byte>& media) {
    std::vector<std::byte> payload{std::byte{0}, std::byte{0}};
    for (int shift = 56; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<std::byte>((peer_timestamp >> shift) & 0xffU));
    }
    payload.insert(payload.end(), media.begin(), media.end());
    return payload;
}

// 9600 bytes = 2400 stereo 16-bit PCM frames (50 ms at 48 kHz), sized so three
// concurrent chunks exceed AudioRenderWorker's 19200-byte per-slot envelope and
// trigger deferred credit waiting while a sink write is held.
std::vector<std::byte> pcm_50ms() {
    return std::vector<std::byte>(9600, std::byte{0x40});
}

struct SinkGate final {
    std::mutex mutex;
    std::condition_variable cv;
    bool block_first_write{false};
    bool released{false};
    bool fail_first_write{false};
    std::size_t fail_write_index{0};
    std::size_t writes_entered{0};
    std::size_t writes_completed{0};
    std::size_t sinks_opened{0};
    std::size_t sinks_destroyed{0};
    std::vector<std::thread::id> write_threads;
    std::vector<std::vector<std::byte>> pcm;
    std::vector<aa::core::Nanoseconds> timestamps;

    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }

    bool wait_entered(std::size_t count,
                      std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return writes_entered >= count; });
    }
};

class GatedAudioSink final : public audio::detail::ObservedAudioSink {
public:
    explicit GatedAudioSink(std::shared_ptr<SinkGate> gate) : gate_(std::move(gate)) {}

    ~GatedAudioSink() override {
        std::lock_guard lock(gate_->mutex);
        ++gate_->sinks_destroyed;
        gate_->cv.notify_all();
    }

    aa::core::Result<void> write(const audio::MixedChunk& chunk) override {
        std::unique_lock lock(gate_->mutex);
        const std::size_t index = ++gate_->writes_entered;
        gate_->write_threads.push_back(std::this_thread::get_id());
        gate_->pcm.emplace_back(chunk.pcm.begin(), chunk.pcm.end());
        gate_->timestamps.push_back(chunk.timestamp);
        gate_->cv.notify_all();

        if (index == 1 && gate_->block_first_write) {
            if (!gate_->cv.wait_for(lock, std::chrono::seconds{5},
                                    [&] { return gate_->released; })) {
                return aa::Error{aa::ErrorCode::timeout};
            }
        }
        if ((index == 1 && gate_->fail_first_write) ||
            (gate_->fail_write_index != 0 && index == gate_->fail_write_index)) {
            return aa::Error{aa::ErrorCode::timeout};
        }
        ++gate_->writes_completed;
        gate_->cv.notify_all();
        return {};
    }

    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    [[nodiscard]] audio::SinkProbeStats probe_stats() const override { return {}; }
    [[nodiscard]] audio::RenderBarrierStats render_stats() const override { return {}; }

private:
    std::shared_ptr<SinkGate> gate_;
};

audio::AudioRenderWorker::OpenRouter fake_router(const std::shared_ptr<SinkGate>& gate) {
    return [gate](const audio::RouterConfig& config) {
        {
            std::lock_guard lock(gate->mutex);
            ++gate->sinks_opened;
            gate->cv.notify_all();
        }
        return audio::detail::open_router_with_sink_for_test(
            config, std::make_unique<GatedAudioSink>(gate));
    };
}

struct StdoutCapture final {
    bool active{false};
    StdoutCapture() {
        testing::internal::CaptureStdout();
        active = true;
    }
    ~StdoutCapture() {
        if (active) {
            (void)testing::internal::GetCapturedStdout();
        }
    }
    std::string stop() {
        active = false;
        return testing::internal::GetCapturedStdout();
    }
    StdoutCapture(const StdoutCapture&) = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;
};

struct SessionHarness final {
    ss::test::MapTrustStore trust;
    ss::test::ManualClock clock;
    wiredtest::CaptureSink diag_capture;
    diag::Diagnostics diagnostics{diag_capture};
    wiredtest::ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}};
    wiredtest::FakeHandshakeCipher* cipher{nullptr};

    void start(wr::Media& media) {
        const aa::trust::PhoneIdentity phone{"wireless-test-phone"};
        trust.set(phone, aa::trust::Decision::approved);
        ASSERT_TRUE(session.configure(media.config().services));
        ASSERT_TRUE(media.bind(session));
        session.attach_transport(&transport);
        auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
        cipher = owned.get();
        session.attach_cipher(std::move(owned));
        ASSERT_TRUE(session.apply(ss::Event::start_discovery));
        ASSERT_TRUE(session.phone_discovered(phone));
        ASSERT_TRUE(session.apply(ss::Event::transport_ready));
        wiredtest::walk_auth(session, transport);
        ASSERT_TRUE(session.authenticated());
        ASSERT_TRUE(session.announce_services());

        for (const std::int32_t service : {99, 100, 101, 102, 104}) {
            transport.push_inbound(wiredtest::open_request_frame(service));
            ASSERT_TRUE(session.poll_transport());
        }
        ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded));
        ASSERT_EQ(session.state(), ss::State::active);

        ASSERT_TRUE(send_service(99, proto::encode(proto::AvSinkSetup{proto::AvSinkCodec::h264}).value()));
        ASSERT_TRUE(send_service(99, proto::encode(proto::AvSinkStart{1, 0}).value()));
        ASSERT_TRUE(send_service(100, proto::encode(proto::AvSinkSetup{}).value()));
        ASSERT_TRUE(send_service(100, proto::encode(proto::AvSinkStart{-7, 0}).value()));
    }

    aa::core::Result<bool> send_service(std::uint8_t channel, std::vector<std::byte> payload) {
        auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) {
            return frames.error();
        }
        for (auto& frame : frames.value()) {
            transport.push_inbound(std::move(frame));
        }
        return session.poll_transport();
    }

    std::size_t count_acks(std::size_t from_index, std::uint8_t channel, std::int32_t session_id) const {
        const auto expected = proto::encode_av_sink_ack(session_id).value();
        std::size_t count = 0;
        const auto& sent = transport.sent_frames();
        for (std::size_t i = from_index; i < sent.size(); ++i) {
            const auto frame = tr::decode_frame(sent[i]);
            if (!frame || frame.value().header.channel != channel) {
                continue;
            }
            const auto plain = cipher->open(frame.value().payload);
            if (plain && plain.value() == expected) {
                ++count;
            }
        }
        return count;
    }

    std::size_t count_ping_responses(std::size_t from_index, std::int64_t timestamp) const {
        std::size_t count = 0;
        const auto& sent = transport.sent_frames();
        for (std::size_t i = from_index; i < sent.size(); ++i) {
            const auto frame = tr::decode_frame(sent[i]);
            if (!frame || frame.value().header.channel != 0) {
                continue;
            }
            std::vector<std::byte> plain;
            if (frame.value().header.encryption == tr::Encryption::encrypted) {
                const auto opened = cipher->open(frame.value().payload);
                if (!opened) {
                    continue;
                }
                plain = opened.value();
            } else {
                plain = frame.value().payload;
            }
            const auto ping = proto::decode_ping_response(plain);
            if (ping && ping.value().timestamp == timestamp) {
                ++count;
            }
        }
        return count;
    }
};

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

TEST(WirelessAudioWorker, StalledSinkDoesNotBlockSessionThreadOrIngress) {
    auto gate = std::make_shared<SinkGate>();
    gate->block_first_write = true;

    audio::AudioRenderWorker audio{wr::audio_config(audio::SinkBackend::pulse),
                                   wr::audio_telemetry(), fake_router(gate)};
    ASSERT_TRUE(audio.ready());

    wr::Media media{audio};
    auto video_pair = videotest::make_pair(ipc::NegotiatedLimits{});
    ASSERT_TRUE(video_pair);
    media.video.install(std::move(video_pair.value().producer));

    SessionHarness h;
    ASSERT_NO_FATAL_FAILURE(h.start(media));
    const std::size_t ack_baseline = h.transport.sent_count();

    // Audio chunk 1 is submitted to the worker and blocks inside SinkGate::write on
    // the render worker thread; poll_transport() must return without waiting for write().
    const auto session_thread = std::this_thread::get_id();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(h.send_service(100, av_data_message(1'000'000'000ULL, pcm_50ms())));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds{250});

    ASSERT_TRUE(gate->wait_entered(1));
    {
        std::lock_guard lock(gate->mutex);
        ASSERT_EQ(gate->write_threads.size(), 1U);
        EXPECT_NE(gate->write_threads.front(), session_thread);
        EXPECT_EQ(gate->writes_completed, 0U);
    }

    // Admission ACK 1 completes while write 1 is blocked on the worker thread.
    ASSERT_TRUE(app::service_audio_completions(h.session, audio));
    EXPECT_EQ(h.count_acks(ack_baseline, 100, -7), 1U);

    // While audio write 1 stays blocked on the worker thread:
    // 1) Control ping still replies immediately on the session thread.
    // 2) Video IDR still traverses VideoOutlet -> VideoConsumer without blocking.
    // 3) Chunk 2 fits the remaining reservation envelope and ACKs; chunk 3 exceeds
    //    the 19200-byte envelope and has its credit deferred until write 1 completes.
    const std::size_t ping_baseline = h.transport.sent_count();
    const auto ping = proto::encode(proto::PingRequest{424242, false, {std::byte{0x11}}});
    ASSERT_TRUE(ping);
    ASSERT_TRUE(h.send_service(0, ping.value()));
    EXPECT_EQ(h.count_ping_responses(ping_baseline, 424242), 1U);

    ASSERT_TRUE(h.send_service(99, av_data_message(1'050'000'000ULL, wiredtest::kVideoIdr)));
    const auto received_video = video_pair.value().consumer.receive(aa::core::Milliseconds{1000});
    ASSERT_TRUE(received_video);
    EXPECT_EQ(received_video.value(), ipc::ReceiveStatus::complete);
    EXPECT_EQ(video_pair.value().consumer.access_unit().payload.size(), wiredtest::kVideoIdr.size());

    ASSERT_TRUE(h.send_service(100, av_data_message(1'050'000'000ULL, pcm_50ms())));
    ASSERT_TRUE(app::service_audio_completions(h.session, audio));
    EXPECT_EQ(h.count_acks(ack_baseline, 100, -7), 2U);

    ASSERT_TRUE(h.send_service(100, av_data_message(1'100'000'000ULL, pcm_50ms())));
    ASSERT_TRUE(app::service_audio_completions(h.session, audio));
    // Credit for chunk 3 remains deferred until chunk 1 clears the sink.
    EXPECT_EQ(h.count_acks(ack_baseline, 100, -7), 2U);

    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(app::service_audio_completions(h.session, audio));
    EXPECT_EQ(h.count_acks(ack_baseline, 100, -7), 3U);

    {
        std::lock_guard lock(gate->mutex);
        EXPECT_EQ(gate->writes_entered, 3U);
        EXPECT_EQ(gate->writes_completed, 3U);
        ASSERT_EQ(gate->pcm.size(), 3U);
        EXPECT_EQ(gate->pcm[0].size(), 9600U);
        EXPECT_GT(gate->pcm[1].size(), 0U);
        EXPECT_LE(gate->pcm[1].size(), 9600U);
        EXPECT_GT(gate->pcm[2].size(), 0U);
        EXPECT_LE(gate->pcm[2].size(), 9600U);
    }

    audio.stop();
    EXPECT_TRUE(audio.finish());
}

TEST(WirelessAudioWorker, StallBypassTriggersAndReopensOnWriteError) {
    wr::running.store(true);
    auto gate = std::make_shared<SinkGate>();
    gate->block_first_write = true;
    gate->fail_first_write = true;

    std::string output;
    bool ready_ok = false;
    bool entered_ok = false;
    bool send1_ok = false;
    bool svc1_ok = false;
    bool send2_ok = false;
    bool svc2_ok = false;
    bool send3_ok = false;
    bool svc3_ok = false;
    bool svc_bypass_ok = false;
    bool idle1_ok = false;
    bool no_failure = false;
    bool send4_ok = false;
    bool idle2_ok = false;
    bool svc4_ok = false;
    bool finish_ok = false;
    std::size_t acks_at_bypass = 0;
    std::size_t acks_after_resume = 0;
    std::size_t ack_baseline = 0;

    {
        StdoutCapture capture;
        diag::StdoutLogScope logging{wr::running};
        audio::AudioRenderWorker audio{
            wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
            fake_router(gate)};
        ready_ok = audio.ready().has_value();
        if (ready_ok) {
            audio::log_sink_selected(audio.sink_report(), "open", wr::audio_telemetry());
            wr::Media media{audio};
            SessionHarness h;
            h.start(media);
            ack_baseline = h.transport.sent_count();

            // Chunks 1 & 2 fill the 19200-byte reservation envelope while chunk 1 blocks in write();
            // chunk 3 exceeds the envelope so its credit is held in deferred_.
            send1_ok = h.send_service(100, av_data_message(1'000'000'000ULL, pcm_50ms())).has_value();
            svc1_ok = app::service_audio_completions(h.session, audio).has_value();
            entered_ok = gate->wait_entered(1);
            send2_ok = h.send_service(100, av_data_message(1'050'000'000ULL, pcm_50ms())).has_value();
            svc2_ok = app::service_audio_completions(h.session, audio).has_value();
            send3_ok = h.send_service(100, av_data_message(1'100'000'000ULL, pcm_50ms())).has_value();
            svc3_ok = app::service_audio_completions(h.session, audio).has_value();

            // Wait past kStallBypassAfterNs (250 ms) on a bounded CV predicate before pumping completions.
            {
                std::unique_lock lock(gate->mutex);
                (void)gate->cv.wait_for(lock, std::chrono::milliseconds{275}, [] { return false; });
            }
            svc_bypass_ok = app::service_audio_completions(h.session, audio).has_value();
            acks_at_bypass = h.count_acks(ack_baseline, 100, -7);

            // Fail the stalled first write -> worker reopens a healthy sink and clears failure.
            gate->release();
            idle1_ok = audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2});
            no_failure = !audio.failure().has_value();

            // Subsequent audio renders normally on the reopened sink.
            send4_ok = h.send_service(100, av_data_message(1'500'000'000ULL, pcm_50ms())).has_value();
            idle2_ok = audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2});
            svc4_ok = app::service_audio_completions(h.session, audio).has_value();
            acks_after_resume = h.count_acks(ack_baseline, 100, -7);
        }
        audio.stop();
        finish_ok = audio.finish().has_value();
        logging.log.finish();
        output = capture.stop();
    }

    EXPECT_TRUE(ready_ok);
    EXPECT_TRUE(entered_ok);
    EXPECT_TRUE(send1_ok);
    EXPECT_TRUE(svc1_ok);
    EXPECT_TRUE(send2_ok);
    EXPECT_TRUE(svc2_ok);
    EXPECT_TRUE(send3_ok);
    EXPECT_TRUE(svc3_ok);
    EXPECT_TRUE(svc_bypass_ok);
    EXPECT_EQ(acks_at_bypass, 3U) << output;
    EXPECT_TRUE(idle1_ok);
    EXPECT_TRUE(no_failure);
    EXPECT_TRUE(send4_ok);
    EXPECT_TRUE(idle2_ok);
    EXPECT_TRUE(svc4_ok);
    EXPECT_EQ(acks_after_resume, 4U) << output;
    EXPECT_TRUE(finish_ok);

    {
        std::lock_guard lock(gate->mutex);
        EXPECT_GE(gate->sinks_opened, 2U);
        EXPECT_GE(gate->sinks_destroyed, 2U);
        EXPECT_GE(gate->writes_completed, 1U);
    }

    EXPECT_NE(output.find("\"event\":\"audio_sink_selected\",\"origin\":\"open\""),
              std::string::npos) << output;
    EXPECT_NE(output.find("\"event\":\"audio_stall_bypass\",\"phase\":\"begin\",\"trigger\":\"credit_wait\""),
              std::string::npos) << output;
    EXPECT_NE(output.find("\"event\":\"audio_sink_selected\",\"origin\":\"reopen\""),
              std::string::npos) << output;
    EXPECT_NE(output.find("\"event\":\"audio_stall_bypass\",\"phase\":\"end\""),
              std::string::npos) << output;
    EXPECT_NE(output.find("\"outcome\":\"sink_reopened\""), std::string::npos) << output;
}

TEST(WirelessAudioWorker, TelemetryEventParityWithCollectorAndSmokeContracts) {
    wr::running.store(true);

    // 1. Verify wireless router configuration matches wired live-PCM drift + backend contract,
    //    with catch-up configurable and off by default (#155).
    const auto cfg = wr::audio_config(audio::SinkBackend::pulse);
    EXPECT_EQ(cfg.backend, audio::SinkBackend::pulse);
    EXPECT_TRUE(cfg.live_pcm_drift.enabled);
    EXPECT_EQ(cfg.live_pcm_drift.catch_up_watermark, aa::core::Nanoseconds{0});
    EXPECT_TRUE(static_cast<bool>(cfg.on_write_completed));
    const auto catch_up_cfg =
        wr::audio_config(audio::SinkBackend::pulse, aa::core::Nanoseconds{90'000'000});
    EXPECT_EQ(catch_up_cfg.live_pcm_drift.catch_up_watermark, aa::core::Nanoseconds{90'000'000});

    // 2. Verify DiagnosticOutput emits {"event":"diag",...} and worker emits exact audio JSON events.
    auto gate = std::make_shared<SinkGate>();
    std::string output;
    bool ready_ok = false;
    bool send1_ok = false;
    bool svc1_ok = false;
    bool send2_ok = false;
    bool idle_ok = false;
    bool svc2_ok = false;
    bool finish_ok = false;
    {
        StdoutCapture capture;
        diag::StdoutLogScope logging{wr::running};
        wr::DiagnosticOutput diag_out{true};
        diag_out.emit_json(
            "{\"kind\":\"session_state_changed\",\"mono_ns\":10,\"to_state\":\"active\"}");

        audio::AudioRenderWorker audio{
            wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
            fake_router(gate)};
        ready_ok = audio.ready().has_value();
        if (ready_ok) {
            audio::log_sink_selected(audio.sink_report(), "open", wr::audio_telemetry());

            wr::Media media{audio};
            SessionHarness h;
            h.start(media);
            send1_ok = h.send_service(100, av_data_message(1'000'000'000ULL, wiredtest::pcm_unit())).has_value();
            svc1_ok = app::service_audio_completions(h.session, audio).has_value();
            send2_ok = h.send_service(100, av_data_message(1'020'000'000ULL, wiredtest::pcm_unit())).has_value();
            idle_ok = audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2});
            svc2_ok = app::service_audio_completions(h.session, audio).has_value();
        }
        audio.stop();
        finish_ok = audio.finish().has_value();
        logging.log.finish();
        output = capture.stop();
    }

    EXPECT_TRUE(ready_ok);
    EXPECT_TRUE(send1_ok);
    EXPECT_TRUE(svc1_ok);
    EXPECT_TRUE(send2_ok);
    EXPECT_TRUE(idle_ok);
    EXPECT_TRUE(svc2_ok);
    EXPECT_TRUE(finish_ok);

    const auto lines = split_lines(output);
    ASSERT_FALSE(lines.empty());
    for (const auto& line : lines) {
        ASSERT_EQ(line.front(), '{') << line;
        ASSERT_EQ(line.back(), '}') << line;
    }

    EXPECT_NE(output.find(
                  "{\"event\":\"diag\",\"kind\":\"session_state_changed\",\"mono_ns\":10,\"to_state\":\"active\"}"),
              std::string::npos) << output;
    EXPECT_NE(output.find(
                  "{\"event\":\"audio_sink_selected\",\"origin\":\"open\",\"requested\":\"pulse\","),
              std::string::npos) << output;
    EXPECT_NE(output.find(
                  "\"event\":\"audio_write_completed\",\"scheduled_ns\":"),
              std::string::npos) << output;
    EXPECT_NE(output.find("\"scheduled_end_ns\":"), std::string::npos) << output;
    EXPECT_NE(output.find("\"completed_ns\":"), std::string::npos) << output;
    EXPECT_NE(output.find("\"presented_ns\":null,\"playback_slot_absence\":\"unavailable\"}"),
              std::string::npos) << output;
    EXPECT_NE(output.find("{\"event\":\"audio_chunks\",\"count\":"),
              std::string::npos) << output;
    EXPECT_NE(output.find("{\"event\":\"media_timing\",\"audio_scheduling_skew_ms\":"),
              std::string::npos) << output;
    EXPECT_NE(output.find(",\"audio_latency_ms\":"), std::string::npos) << output;

    // 3. Direct parity check for populated presented_ns and #155 catch-up telemetry seam.
    audio::WriteCompletion sample{};
    sample.scheduled = aa::core::Nanoseconds{1000};
    sample.scheduled_end = aa::core::Nanoseconds{21000};
    sample.completed = aa::core::Nanoseconds{20500};
    sample.presented = aa::core::Nanoseconds{22000};
    EXPECT_EQ(
        audio::audio_write_completed_json(sample),
        "{\"event\":\"audio_write_completed\",\"scheduled_ns\":1000,"
        "\"scheduled_end_ns\":21000,\"completed_ns\":20500,"
        "\"presented_ns\":22000,\"playback_slot_absence\":null}");
    EXPECT_EQ(
        audio::audio_catch_up_json(960, 1920, 48000, 3, 4800),
        "{\"event\":\"audio_catch_up\",\"frames\":960,\"frames_total\":1920,\"rate\":48000,"
        "\"events_lifetime\":3,\"frames_lifetime\":4800}");
}

TEST(WirelessAudioWorker, ReconnectResetsGenerationAndFailureSurfacesPollErrorJson) {
    auto gate = std::make_shared<SinkGate>();
    audio::AudioRenderWorker audio{
        wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(), fake_router(gate)};
    ASSERT_TRUE(audio.ready());

    wr::Media media{audio};

    // First wireless connection delivers an audio frame.
    {
        SessionHarness h1;
        ASSERT_NO_FATAL_FAILURE(h1.start(media));
        ASSERT_TRUE(h1.send_service(100, av_data_message(1'000'000'000ULL, wiredtest::pcm_unit())));
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        ASSERT_TRUE(app::service_audio_completions(h1.session, audio));
    }

    // Reconnect: a second WiredSession binds the same Media / AudioRenderWorker.
    // begin_session() inside Media::bind() resets stale per-session generation/rejection state.
    SessionHarness h2;
    ASSERT_NO_FATAL_FAILURE(h2.start(media));
    const std::size_t ack_baseline = h2.transport.sent_count();
    ASSERT_TRUE(h2.send_service(100, av_data_message(2'000'000'000ULL, wiredtest::pcm_unit())));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(app::service_audio_completions(h2.session, audio));
    EXPECT_EQ(h2.count_acks(ack_baseline, 100, -7), 1U);

    {
        std::lock_guard lock(gate->mutex);
        EXPECT_EQ(gate->writes_completed, 2U);
    }

    // Failure forensics parity: set active media_audio generation deterministically
    // and submit a negative timestamp to latch audio_sink_failure with full context.
    const auto role = proto::ChannelRole::media_audio;
    audio.invalidate(role, 99);
    ASSERT_TRUE(audio.submit({{role, 99, {-1LL}}, wiredtest::pcm_unit()}, {role, 99, 99, 2}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.failure());
    EXPECT_EQ(audio.failure()->code(), aa::ErrorCode::invalid_argument);
    const auto failure_json = audio::audio_failure_event_json(
        "audio_sink_failure", audio.failure()->code(), audio.failure_context());
    EXPECT_NE(failure_json.find("\"event\":\"err\",\"detail\":\"audio_sink_failure\","),
              std::string::npos) << failure_json;
    EXPECT_NE(failure_json.find("\"audio_failure\":{"), std::string::npos) << failure_json;

    // Wireless post-stop poll_error_json overload formats ipc_queue_full cleanly without a live session.
    const auto poll_json = app::poll_error_json(aa::ErrorCode::ipc_queue_full, audio);
    EXPECT_NE(poll_json.find("\"event\":\"err\",\"detail\":\"poll\",\"code\":\"ipc_queue_full\""),
              std::string::npos) << poll_json;

    audio.stop();
    EXPECT_FALSE(audio.finish());
}

TEST(WirelessAudioWorker, SessionResetClearsPriorBypassAndProtectsNewPcmFromLateWriteError) {
    // 1. Existing stall bypass in epoch 1 must not drop or prematurely ACK epoch 2 PCM
    //    submitted after begin_session() with reused identity while write 1 is still blocked.
    {
        wr::running.store(true);
        auto gate = std::make_shared<SinkGate>();
        gate->block_first_write = true;

        std::string output;
        bool ready_ok = false;
        bool idle_ok = false;
        bool finish_ok = false;
        {
            StdoutCapture capture;
            diag::StdoutLogScope logging{wr::running};
            audio::AudioRenderWorker audio{
                wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
                fake_router(gate)};
            ready_ok = audio.ready().has_value();
            if (ready_ok) {
                wr::Media media{audio};
                SessionHarness h1;
                h1.start(media);
                const std::size_t h1_base = h1.transport.sent_count();
                EXPECT_TRUE(h1.send_service(100, av_data_message(2'000'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h1.session, audio));
                EXPECT_TRUE(gate->wait_entered(1));
                EXPECT_TRUE(h1.send_service(100, av_data_message(2'050'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h1.session, audio));
                EXPECT_TRUE(h1.send_service(100, av_data_message(2'100'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h1.session, audio));
                {
                    std::unique_lock lock(gate->mutex);
                    (void)gate->cv.wait_for(lock, std::chrono::milliseconds{275}, [] { return false; });
                }
                EXPECT_TRUE(app::service_audio_completions(h1.session, audio));
                EXPECT_EQ(h1.count_acks(h1_base, 100, -7), 3U);

                // Epoch 2 starts with the same AV session ID (-7) and earlier timestamp while
                // epoch 1's blocked sink write is still held inside GatedAudioSink::write().
                SessionHarness h2;
                h2.start(media);
                const std::size_t h2_base = h2.transport.sent_count();
                EXPECT_TRUE(h2.send_service(100, av_data_message(1'000'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h2.session, audio));
                EXPECT_EQ(h2.count_acks(h2_base, 100, -7), 1U);

                gate->release();
                idle_ok = audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2});
                EXPECT_TRUE(app::service_audio_completions(h2.session, audio));
                EXPECT_FALSE(audio.failure().has_value());
            }
            audio.stop();
            finish_ok = audio.finish().has_value();
            logging.log.finish();
            output = capture.stop();
        }
        EXPECT_TRUE(ready_ok);
        EXPECT_TRUE(idle_ok);
        EXPECT_TRUE(finish_ok);
        std::lock_guard lock(gate->mutex);
        EXPECT_EQ(gate->writes_entered, 2U) << output;
        EXPECT_EQ(gate->writes_completed, 2U) << output;
    }

    // 2. Without a preexisting bypass, a late sink_write_error from retired epoch 1 must
    //    still reopen the sink without calling enter_bypass_locked() on epoch 2's queue.
    {
        wr::running.store(true);
        auto gate = std::make_shared<SinkGate>();
        gate->block_first_write = true;
        gate->fail_first_write = true;

        std::string output;
        bool ready_ok = false;
        bool idle_ok = false;
        bool finish_ok = false;
        std::size_t acks_before_release = 0;
        std::size_t acks_after_reopen = 0;
        {
            StdoutCapture capture;
            diag::StdoutLogScope logging{wr::running};
            audio::AudioRenderWorker audio{
                wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
                fake_router(gate)};
            ready_ok = audio.ready().has_value();
            if (ready_ok) {
                wr::Media media{audio};
                SessionHarness h1;
                h1.start(media);
                EXPECT_TRUE(h1.send_service(100, av_data_message(2'000'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h1.session, audio));
                EXPECT_TRUE(gate->wait_entered(1));

                // Epoch 2 queues 3 chunks (chunk 3 deferred) before retired write 1 fails.
                SessionHarness h2;
                h2.start(media);
                const std::size_t h2_base = h2.transport.sent_count();
                EXPECT_TRUE(h2.send_service(100, av_data_message(1'000'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h2.session, audio));
                EXPECT_TRUE(h2.send_service(100, av_data_message(1'050'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h2.session, audio));
                EXPECT_TRUE(h2.send_service(100, av_data_message(1'100'000'000ULL, pcm_50ms())));
                EXPECT_TRUE(app::service_audio_completions(h2.session, audio));
                acks_before_release = h2.count_acks(h2_base, 100, -7);

                gate->release();
                idle_ok = audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2});
                EXPECT_TRUE(app::service_audio_completions(h2.session, audio));
                acks_after_reopen = h2.count_acks(h2_base, 100, -7);
                EXPECT_FALSE(audio.failure().has_value());
            }
            audio.stop();
            finish_ok = audio.finish().has_value();
            logging.log.finish();
            output = capture.stop();
        }
        EXPECT_TRUE(ready_ok);
        EXPECT_EQ(acks_before_release, 2U);
        EXPECT_TRUE(idle_ok);
        EXPECT_EQ(acks_after_reopen, 3U);
        EXPECT_TRUE(finish_ok);
        EXPECT_EQ(output.find("\"trigger\":\"sink_write_error\""), std::string::npos) << output;
        std::lock_guard lock(gate->mutex);
        EXPECT_GE(gate->sinks_opened, 2U);
        EXPECT_EQ(gate->writes_entered, 4U) << output;
        EXPECT_EQ(gate->writes_completed, 3U) << output;
    }
}

// #155 F2/F4: normal wireless TCP coalesced delivery (3 x 50 ms chunks within
// 5 ms) must not be trimmed under the production wireless default (catch-up off),
// while configuring the 90 ms watermark trims a post-stall burst and survives.
TEST(WirelessAudioWorker, NormalCoalescedBurstIsNotTrimmedAndPostStallBurstCatchesUpWhenEnabled) {
    // 1. Production wireless default (catch-up off): 3 x 50 ms chunks arriving
    //    within 2 ms produce zero catch-up loss and deliver all three writes.
    {
        wr::running.store(true);
        auto gate = std::make_shared<SinkGate>();
        std::string output;
        std::optional<audio::RouterReport> report;
        bool finish_ok = false;
        {
            StdoutCapture capture;
            diag::StdoutLogScope logging{wr::running};
            audio::AudioRenderWorker audio{
                wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
                fake_router(gate)};
            ASSERT_TRUE(audio.ready());
            wr::Media media{audio};
            SessionHarness h;
            ASSERT_NO_FATAL_FAILURE(h.start(media));
            for (std::uint64_t i = 0; i < 3; ++i) {
                ASSERT_TRUE(h.send_service(100, av_data_message(1'000'000'000ULL + i * 50'000'000ULL,
                                                                pcm_50ms())));
                ASSERT_TRUE(app::service_audio_completions(h.session, audio));
                h.clock.advance(aa::core::Milliseconds{1});
            }
            ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
            ASSERT_TRUE(app::service_audio_completions(h.session, audio));
            EXPECT_FALSE(audio.failure().has_value());
            audio.stop();
            finish_ok = audio.finish().has_value();
            report = audio.final_report();
            logging.log.finish();
            output = capture.stop();
        }
        EXPECT_TRUE(finish_ok) << output;
        ASSERT_TRUE(report.has_value());
        EXPECT_EQ(report->stats.frames_caught_up, 0U) << output;
        EXPECT_EQ(output.find("\"event\":\"audio_catch_up\""), std::string::npos) << output;
        std::lock_guard lock(gate->mutex);
        EXPECT_EQ(gate->writes_completed, 3U);
    }

    // 2. Wireless with catch-up configured on (90 ms): a 16-chunk post-stall
    //    burst stamped at one arrival is trimmed instead of ending the session.
    {
        wr::running.store(true);
        auto gate = std::make_shared<SinkGate>();
        std::string output;
        std::optional<audio::RouterReport> report;
        bool finish_ok = false;
        aa::core::Nanoseconds arrival{};
        {
            StdoutCapture capture;
            diag::StdoutLogScope logging{wr::running};
            audio::AudioRenderWorker audio{
                wr::audio_config(audio::SinkBackend::pulse, aa::core::Nanoseconds{90'000'000}),
                wr::audio_telemetry(), fake_router(gate)};
            ASSERT_TRUE(audio.ready());
            wr::Media media{audio};
            SessionHarness h;
            ASSERT_NO_FATAL_FAILURE(h.start(media));
            arrival = h.clock.now();
            const std::vector<std::byte> chunk_42ms(8192, std::byte{0x22});
            for (std::uint64_t i = 0; i < 16; ++i) {
                ASSERT_TRUE(h.send_service(100, av_data_message(1'000'000'000ULL + i * 42'666'667ULL,
                                                                chunk_42ms)));
                ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
                ASSERT_TRUE(app::service_audio_completions(h.session, audio));
                ASSERT_FALSE(audio.failure().has_value()) << i;
            }
            h.clock.advance(aa::core::Milliseconds{100});
            ASSERT_TRUE(h.send_service(100, av_data_message(2'000'000'000ULL, chunk_42ms)));
            ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
            ASSERT_TRUE(app::service_audio_completions(h.session, audio));
            EXPECT_FALSE(audio.failure().has_value());
            audio.stop();
            finish_ok = audio.finish().has_value();
            report = audio.final_report();
            logging.log.finish();
            output = capture.stop();
        }
        EXPECT_TRUE(finish_ok) << output;
        EXPECT_EQ(output.find("\"event\":\"err\""), std::string::npos) << output;
        ASSERT_TRUE(report.has_value());
        EXPECT_FALSE(report->render_failure.has_value());
        EXPECT_GT(report->stats.frames_caught_up, 0U);
        const std::regex catch_up{
            R"(\{"event":"audio_catch_up","frames":([0-9]+),"frames_total":([0-9]+),"rate":48000,"events_lifetime":([0-9]+),"frames_lifetime":([0-9]+)\})"};
        std::uint64_t logged = 0;
        std::uint64_t last_total = 0;
        std::size_t events = 0;
        for (auto it = std::sregex_iterator(output.begin(), output.end(), catch_up);
             it != std::sregex_iterator{}; ++it) {
            logged += std::stoull((*it)[1].str());
            last_total = std::stoull((*it)[2].str());
            ++events;
            EXPECT_EQ(std::stoull((*it)[3].str()), events) << output;
            EXPECT_EQ(std::stoull((*it)[4].str()), logged) << output;
        }
        EXPECT_GT(events, 0U) << output;
        EXPECT_EQ(logged, report->stats.frames_caught_up) << output;
        EXPECT_EQ(last_total, report->stats.frames_caught_up) << output;
        std::lock_guard lock(gate->mutex);
        ASSERT_FALSE(gate->timestamps.empty());
        EXPECT_LE(gate->timestamps.back().count, arrival.count + 100'000'000 + 90'000'000);
    }
}

// #155 F4: worker-lifetime catch-up totals (events_lifetime, frames_lifetime)
// continue monotonically across a real sink_write_error + sink reopen while
// per-router frames_total resets on the reopened AudioRouter.
TEST(WirelessAudioWorker, CatchUpLifetimeTotalsSurviveRealSinkReopen) {
    wr::running.store(true);
    auto gate = std::make_shared<SinkGate>();
    std::string output;
    std::optional<audio::RouterReport> report;
    bool finish_ok = false;
    {
        StdoutCapture capture;
        diag::StdoutLogScope logging{wr::running};
        audio::AudioRenderWorker audio{
            wr::audio_config(audio::SinkBackend::pulse, aa::core::Nanoseconds{90'000'000}),
            wr::audio_telemetry(), fake_router(gate)};
        ASSERT_TRUE(audio.ready());
        const auto role = proto::ChannelRole::media_audio;
        const std::vector<std::byte> chunk(8192, std::byte{0x33}); // 2048 frames (42.67 ms)
        const auto base = audio::AudioRenderWorker::monotonic_now().count;
        std::uint64_t seq = 1;

        // Burst 1: 6 chunks at base -> chunks 4-6 caught up (3 events, 3 * 1808 = 5424 frames).
        for (int i = 0; i < 6; ++i, ++seq) {
            ASSERT_TRUE(audio.submit({{role, seq, {base}}, chunk}, {role, 0, seq, 7}));
            ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
            ASSERT_FALSE(audio.failure().has_value()) << seq;
        }

        // Force a real sink_write_error on the next write -> worker reopens a fresh sink.
        {
            std::lock_guard lock(gate->mutex);
            gate->fail_write_index = gate->writes_entered + 1;
        }
        ASSERT_TRUE(audio.submit({{role, seq, {base + 100'000'000}}, chunk}, {role, 0, seq, 7}));
        ++seq;
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        ASSERT_FALSE(audio.failure().has_value());
        {
            std::lock_guard lock(gate->mutex);
            ASSERT_GE(gate->sinks_opened, 2U);
            gate->fail_write_index = 0;
        }

        // Burst 2 on the reopened router: 6 chunks at base + 300 ms -> 3 more events (5424 frames).
        for (int i = 0; i < 6; ++i, ++seq) {
            ASSERT_TRUE(audio.submit({{role, seq, {base + 300'000'000}}, chunk}, {role, 0, seq, 7}));
            ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
            ASSERT_FALSE(audio.failure().has_value()) << seq;
        }

        audio.stop();
        finish_ok = audio.finish().has_value();
        report = audio.final_report();
        logging.log.finish();
        output = capture.stop();
    }

    EXPECT_TRUE(finish_ok) << output;
    EXPECT_EQ(output.find("\"event\":\"err\""), std::string::npos) << output;
    EXPECT_NE(output.find("\"event\":\"audio_sink_selected\",\"origin\":\"reopen\""),
              std::string::npos) << output;
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->stats.frames_caught_up, 3U * 1808U);

    struct Record {
        std::uint64_t frames;
        std::uint64_t frames_total;
        std::uint64_t events_lifetime;
        std::uint64_t frames_lifetime;
    };
    const std::regex catch_up{
        R"(\{"event":"audio_catch_up","frames":([0-9]+),"frames_total":([0-9]+),"rate":48000,"events_lifetime":([0-9]+),"frames_lifetime":([0-9]+)\})"};
    std::vector<Record> records;
    for (auto it = std::sregex_iterator(output.begin(), output.end(), catch_up);
         it != std::sregex_iterator{}; ++it) {
        records.push_back({std::stoull((*it)[1].str()), std::stoull((*it)[2].str()),
                           std::stoull((*it)[3].str()), std::stoull((*it)[4].str())});
    }
    ASSERT_EQ(records.size(), 6U) << output;
    // Before reopen: frames_total tracks frames_lifetime (1808, 3616, 5424).
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(records[i].frames, 1808U);
        EXPECT_EQ(records[i].frames_total, (i + 1) * 1808U);
        EXPECT_EQ(records[i].events_lifetime, i + 1);
        EXPECT_EQ(records[i].frames_lifetime, (i + 1) * 1808U);
    }
    // After reopen: frames_total resets (1808, 3616, 5424) while lifetime totals keep advancing.
    for (std::size_t i = 3; i < 6; ++i) {
        EXPECT_EQ(records[i].frames, 1808U);
        EXPECT_EQ(records[i].frames_total, (i - 2) * 1808U);
        EXPECT_LT(records[i].frames_total, records[i].frames_lifetime);
        EXPECT_EQ(records[i].events_lifetime, i + 1);
        EXPECT_EQ(records[i].frames_lifetime, (i + 1) * 1808U);
    }
}

} // namespace
