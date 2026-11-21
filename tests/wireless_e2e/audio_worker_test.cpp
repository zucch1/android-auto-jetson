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

#include <deque>
#include <optional>
#include <array>
#include <algorithm>
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
        return count_payload(from_index, channel, proto::encode_av_sink_ack(session_id).value());
    }

    std::size_t count_payload(std::size_t from_index, std::uint8_t channel,
                              const std::vector<std::byte>& expected) const {
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
    // Credit window + playout: the production default plays on the playout
    // clock; catch-up is only reachable on the arrival model (playout 0).
    EXPECT_TRUE(cfg.playout.enabled);
    EXPECT_EQ(cfg.playout.depth, audio::kWirelessPlayoutDepth);
    const auto catch_up_cfg = wr::audio_config(audio::SinkBackend::pulse,
                                               aa::core::Nanoseconds{90'000'000}, aa::core::Nanoseconds{0});
    EXPECT_EQ(catch_up_cfg.live_pcm_drift.catch_up_watermark, aa::core::Nanoseconds{90'000'000});
    EXPECT_FALSE(catch_up_cfg.playout.enabled);

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
    // Window 8 lost 31.8 s to trims and played 32.1 s of gap silence, but only
    // the 10.6 s caught up was ever logged: media_timing carries the router's
    // cumulative loss and silence so a live window shows them directly.
    audio::RouterStats timing{};
    timing.max_completion_skew = aa::core::Nanoseconds{2'000'000};
    timing.max_path_latency = aa::core::Nanoseconds{5'500'000};
    timing.frames_decoded = 11'534'336;
    timing.frames_trimmed = 1'526'736;
    timing.frames_caught_up = 509'856;
    timing.frames_silence = 1'542'356;
    timing.frames_idle_skipped = 17'675;
    EXPECT_EQ(
        audio::media_timing_json(timing),
        "{\"event\":\"media_timing\",\"audio_scheduling_skew_ms\":2,"
        "\"audio_latency_ms\":5.5,\"frames_decoded\":11534336,\"frames_trimmed\":1526736,"
        "\"frames_caught_up\":509856,\"frames_silence\":1542356,\"frames_skipped\":0,"
        "\"frames_idle_skipped\":17675,\"sink_reopens\":0}");
    // Wireless playout: the buffer target, depth, underruns and the credit
    // window the phone actually used, over every sink route.
    timing.frames_skipped = 4800;
    timing.playout_depth = aa::core::Nanoseconds{118'500'000};
    timing.playout_depth_min = aa::core::Nanoseconds{61'000'000};
    timing.playout_depth_max = aa::core::Nanoseconds{124'000'000};
    timing.playout_offset = aa::core::Nanoseconds{121'000'000};
    timing.playout_underruns = 1;
    timing.playout_anchors = 2;
    timing.frames_padded = 240;
    timing.playout_resumes = 1;
    timing.playout_reported_offset = aa::core::Nanoseconds{119'000'000};
    timing.frames_shed = 96;
    timing.max_arrival_burst = 3;
    audio::MediaTimingContext context;
    context.sink_reopens = 1;
    context.playout_target = aa::core::Nanoseconds{120'000'000};
    context.credit_window = 4;
    context.max_unacked_held = 3;
    EXPECT_EQ(
        audio::media_timing_json(timing, context),
        "{\"event\":\"media_timing\",\"audio_scheduling_skew_ms\":2,"
        "\"audio_latency_ms\":5.5,\"frames_decoded\":11534336,\"frames_trimmed\":1526736,"
        "\"frames_caught_up\":509856,\"frames_silence\":1542356,\"frames_skipped\":4800,"
        "\"frames_idle_skipped\":17675,\"sink_reopens\":1,\"playout_target_ms\":120,"
        "\"playout_depth_ms\":118.5,\"playout_depth_min_ms\":61,\"playout_depth_max_ms\":124,"
        "\"playout_offset_ms\":121,\"playout_reported_offset_ms\":119,\"playout_underruns\":1,"
        "\"playout_anchors\":2,\"playout_resumes\":1,\"frames_padded\":240,\"frames_shed\":96,"
        "\"credit_window\":4,"
        "\"max_unacked_held\":3,\"max_arrival_burst\":3}");
    // Lifetime: a reopened route's counters add to the earlier routes'.
    audio::RouterStats earlier{};
    earlier.frames_decoded = 100;
    earlier.frames_trimmed = 7;
    earlier.playout_underruns = 2;
    earlier.max_path_latency = aa::core::Nanoseconds{9'000'000};
    earlier.playout_depth_min = aa::core::Nanoseconds{40'000'000};
    const auto total = audio::accumulate_router_stats(earlier, timing);
    EXPECT_EQ(total.frames_decoded, 11'534'436U);
    EXPECT_EQ(total.frames_trimmed, 1'526'743U);
    EXPECT_EQ(total.playout_underruns, 3U);
    EXPECT_EQ(total.max_path_latency, aa::core::Nanoseconds{9'000'000});
    EXPECT_EQ(total.playout_depth_min, aa::core::Nanoseconds{40'000'000});
    EXPECT_EQ(total.playout_depth, timing.playout_depth);
    // A/V: a playout write reports its content (arrival-clock) times and the
    // offset; the arrival model's record is unchanged.
    audio::WriteCompletion played{aa::core::Nanoseconds{121'000}, aa::core::Nanoseconds{141'000},
                                  aa::core::Nanoseconds{20'500}, aa::core::Nanoseconds{122'000},
                                  audio::PlaybackSlotAbsence::none, aa::core::Nanoseconds{120'000},
                                  aa::core::Nanoseconds{120'000}};
    EXPECT_EQ(audio::audio_write_completed_json(played),
              "{\"event\":\"audio_write_completed\",\"scheduled_ns\":1000,"
              "\"scheduled_end_ns\":21000,\"playout_offset_ns\":120000,\"completed_ns\":20500,"
              "\"presented_ns\":122000,\"playback_slot_absence\":null}");
    // While an offset increase is absorbed the content interval is shorter.
    played.playout_end_offset = aa::core::Nanoseconds{130'000};
    EXPECT_NE(audio::audio_write_completed_json(played).find(
                  "\"scheduled_ns\":1000,\"scheduled_end_ns\":11000,\"playout_offset_ns\":120000,"),
              std::string::npos);
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
                wr::audio_config(audio::SinkBackend::pulse, aa::core::Nanoseconds{90'000'000},
                                 aa::core::Nanoseconds{0}),
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
            wr::audio_config(audio::SinkBackend::pulse, aa::core::Nanoseconds{90'000'000},
                                 aa::core::Nanoseconds{0}),
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


// ---- Credit window + playout (owner decision 2026-11-20) --------------------

// One 2048-frame A26 media chunk (42.67 ms of 48 kHz stereo PCM).
std::vector<std::byte> pcm_chunk_2048() { return std::vector<std::byte>(8192, std::byte{0x20}); }

constexpr std::int64_t kChunkNs = 2048LL * 1'000'000'000LL / 48000LL;

struct CollectedTelemetry final {
    std::mutex mutex;
    std::vector<std::string> lines;
    std::vector<std::string> kinds;

    audio::AudioTelemetryCallbacks callbacks() {
        const auto line = [this](const std::string& value) {
            std::lock_guard lock(mutex);
            lines.push_back(value);
            kinds.emplace_back("line");
        };
        return {line,
                [this](std::string_view kind, const std::string& value) {
                    std::lock_guard lock(mutex);
                    lines.push_back(value);
                    kinds.emplace_back(kind);
                },
                line};
    }

    [[nodiscard]] std::vector<std::string> of(std::string_view kind) {
        std::lock_guard lock(mutex);
        std::vector<std::string> out;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            if (kinds[i] == kind) { out.push_back(lines[i]); }
        }
        return out;
    }
};

// The production wireless policy (wr::audio_config(): playout, 120 ms) and
// credit window, with write completions kept in the test instead of stdout.
[[nodiscard]] audio::RouterConfig quiet_wireless_config() {
    auto config = wr::audio_config(audio::SinkBackend::pulse);
    config.on_write_completed = {};
    return config;
}

TEST(WirelessAudioWorker, CreditWindowAdvertisesFourAndKeepsFourChunksInFlight) {
    auto gate = std::make_shared<SinkGate>();
    gate->block_first_write = true;
    CollectedTelemetry telemetry;
    audio::AudioRenderWorker audio{quiet_wireless_config(), telemetry.callbacks(), fake_router(gate),
                                   audio::AudioRenderWorker::monotonic_now,
                                   audio::AudioRenderWorker::kDefaultAckReleaseDrainTimeout,
                                   audio::AudioCreditPolicy{audio::kWirelessCreditWindow}};
    ASSERT_TRUE(audio.ready());
    EXPECT_EQ(audio.credit_window(), 4U);
    wr::Media media{audio};
    SessionHarness h;
    ASSERT_NO_FATAL_FAILURE(h.start(media));
    // The media channel's ready advertised max_unacked 4 (the A26 logs it as
    // "maxUnackedFramesAllowed").
    const std::array<std::uint32_t, 1> indices{0};
    EXPECT_EQ(h.count_payload(0, 100, proto::encode_av_sink_ready(indices, 4).value()), 1U);
    EXPECT_EQ(h.count_payload(0, 100, proto::encode_av_sink_ready().value()), 0U);
    const std::size_t ack_baseline = h.transport.sent_count();

    // The phone sends four chunks without waiting; the first write blocks.
    for (int i = 0; i < 4; ++i) {
        h.clock.set(aa::core::Nanoseconds{1'000'000'000LL + i * kChunkNs});
        ASSERT_TRUE(h.send_service(100, av_data_message(static_cast<std::uint64_t>(i), pcm_chunk_2048())));
    }
    ASSERT_TRUE(gate->wait_entered(1));
    EXPECT_EQ(audio.max_unacked_held(), 4U);
    // All four fit the scaled reservation: each is acked while write 1 is held.
    ASSERT_TRUE(app::service_audio_completions(h.session, audio));
    EXPECT_EQ(h.count_acks(ack_baseline, 100, -7), 4U);
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    audio.stop();
    EXPECT_TRUE(audio.finish());
    ASSERT_TRUE(audio.final_report());
    EXPECT_EQ(audio.final_report()->stats.frames_retained, 4U * 2048U);
    EXPECT_EQ(audio.final_report()->stats.frames_trimmed, 0U);
}

// The same worker with stop-and-wait keeps the old reservation exactly.
TEST(WirelessAudioWorker, StopAndWaitCreditPolicyAdvertisesOne) {
    auto gate = std::make_shared<SinkGate>();
    audio::AudioRenderWorker audio{quiet_wireless_config(), {}, fake_router(gate)};
    ASSERT_TRUE(audio.ready());
    EXPECT_EQ(audio.credit_window(), 1U);
    wr::Media media{audio};
    SessionHarness h;
    ASSERT_NO_FATAL_FAILURE(h.start(media));
    EXPECT_EQ(h.count_payload(0, 100, proto::encode_av_sink_ready().value()), 1U);
    audio.stop();
    EXPECT_TRUE(audio.finish());
}

struct WorkerReplay final {
    audio::RouterStats stats{};
    std::optional<aa::ErrorCode> failure{};
    std::uint64_t chunks{};
    std::vector<std::string> media_timing{};
    std::vector<std::string> underruns{};
    std::vector<std::string> anchors{};
    std::uint64_t max_unacked_held{};
    std::int64_t playout_offset{};
    // Every write's report, and (content time, hold the video source gave)
    // for a frame arriving with each chunk.
    std::vector<audio::WriteCompletion> completions{};
    std::vector<std::pair<std::int64_t, std::chrono::nanoseconds>> holds{};
    // Inside recorded gaps of the last 8 s: (lookup, the line between the
    // neighbouring writes' offsets, that line's slope x 1000).
    struct GapSample final {
        std::int64_t lookup;
        std::int64_t expected;
        std::int64_t slope_milli;
    };
    std::vector<GapSample> gaps{};
    // After the run: the session reset and the hold fell back to the depth.
    std::chrono::nanoseconds hold_after_reset{};
    std::chrono::nanoseconds offset_after_reset{};
};

// The reported playout offset for content time `t`, from the write records.
[[nodiscard]] std::optional<std::int64_t> reported_offset_at(
    const std::vector<audio::WriteCompletion>& completions, std::int64_t t) {
    for (const auto& c : completions) {
        const auto start = c.scheduled.count - c.playout_offset.count;
        const auto end = c.scheduled_end.count - c.playout_end_offset.count;
        if (t < start || t >= end) { continue; }
        return c.playout_offset.count
            + (c.playout_end_offset.count - c.playout_offset.count) * (t - start) / std::max<std::int64_t>(1, end - start);
    }
    return std::nullopt;
}

// Window-9-style closed credit loop (see audio_pipeline_test run_playout_loop)
// through the real session, AvSink, render worker and router with the
// production wireless policy. Arrival stamps are the session clock. The phone
// releases chunks in groups of `burst`; a group arriving together is
// submitted before the receiver services any completion, so several chunks
// are really in flight at once.
template <typename RoundTrip>
[[nodiscard]] WorkerReplay replay_through_worker(std::int64_t seconds, RoundTrip round_trip_ms,
                                                 std::uint64_t burst = 1, std::uint64_t later_burst = 0,
                                                 std::int64_t burst_change_s = 0) {
    constexpr std::int64_t kStart = 1'000'000'000;
    WorkerReplay replay;
    auto gate = std::make_shared<SinkGate>();
    CollectedTelemetry telemetry;
    auto completions = std::make_shared<std::vector<audio::WriteCompletion>>();
    auto config = quiet_wireless_config();
    config.on_write_completed = [completions](const audio::WriteCompletion& c) { completions->push_back(c); };
    audio::AudioRenderWorker audio{config, telemetry.callbacks(), fake_router(gate),
                                   audio::AudioRenderWorker::monotonic_now,
                                   audio::AudioRenderWorker::kDefaultAckReleaseDrainTimeout,
                                   audio::AudioCreditPolicy{audio::kWirelessCreditWindow}};
    if (!audio.ready()) { replay.failure = aa::ErrorCode::audio_sink_unavailable; return replay; }
    wr::Media media{audio};
    media.match_video_to_audio(std::chrono::milliseconds{120});
    SessionHarness h;
    h.start(media);
    const auto chunk = pcm_chunk_2048();
    std::deque<std::int64_t> acked_at;
    std::int64_t last_arrival = 0;
    std::deque<std::int64_t> frames;
    std::size_t unserviced = 0;
    std::size_t baseline = h.transport.sent_count();
    // Renders and acks everything submitted so far; each chunk must get its ack.
    const auto service = [&]() -> bool {
        if (unserviced == 0) { return true; }
        if (!audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2})) {
            replay.failure = aa::ErrorCode::timeout;
            return false;
        }
        if (auto serviced = app::service_audio_completions(h.session, audio); !serviced) {
            replay.failure = serviced.error().code();
            return false;
        }
        if (h.count_acks(baseline, 100, -7) != unserviced) {
            replay.failure = aa::ErrorCode::internal;
            return false;
        }
        unserviced = 0;
        baseline = h.transport.sent_count();
        return true;
    };
    const auto change_chunk = static_cast<std::uint64_t>(burst_change_s * 1'000'000'000LL / kChunkNs);
    for (std::uint64_t k = 0;; ++k) {
        auto released = (k / burst) * burst + burst - 1;
        if (later_burst != 0 && k >= change_chunk) {
            const auto j = k - change_chunk;
            released = change_chunk + (j / later_burst) * later_burst + later_burst - 1;
        } else if (later_burst != 0) {
            released = std::min<std::uint64_t>(released, change_chunk - 1);
        }
        const auto captured = static_cast<std::int64_t>(released) * kChunkNs;
        std::int64_t sent = captured;
        if (acked_at.size() >= audio::kWirelessCreditWindow) {
            sent = std::max(sent, acked_at.front());
            acked_at.pop_front();
        }
        if (sent >= seconds * 1'000'000'000LL) { break; }
        const std::int64_t rtt = round_trip_ms(sent / 1'000'000'000LL) * 1'000'000LL;
        const std::int64_t arrival = std::max(last_arrival, kStart + sent + rtt / 2);
        if (arrival > last_arrival && !service()) { break; }
        if (arrival > last_arrival && last_arrival != 0) {
            // A video frame arriving with this chunk is held for the offset of
            // the audio it pairs with (content time = its own timestamp),
            // decided when the outlet checks it for release ~a hold later.
            frames.push_back(last_arrival);
            while (!frames.empty() && frames.front() + 200'000'000 <= arrival) {
                replay.holds.emplace_back(frames.front(),
                    media.video.presentation_delay_for(aa::core::Nanoseconds{frames.front()}));
                frames.pop_front();
            }
        }
        last_arrival = arrival;
        h.clock.set(aa::core::Nanoseconds{arrival});
        if (auto sent_ok = h.send_service(100, av_data_message(k, chunk)); !sent_ok) {
            replay.failure = sent_ok.error().code();
            break;
        }
        ++replay.chunks;
        ++unserviced;
        // The worker acks at arrival: the credit reaches the phone half a
        // round trip later.
        acked_at.push_back(arrival - kStart + (rtt - rtt / 2));
    }
    if (!replay.failure) { (void)service(); }
    replay.max_unacked_held = audio.max_unacked_held();
    replay.playout_offset = audio.current_playout_offset().count();
    for (std::size_t i = 1; i < completions->size(); ++i) {
        const auto& a = (*completions)[i - 1];
        const auto& b = (*completions)[i];
        const auto a_end = a.scheduled_end.count - a.playout_end_offset.count;
        const auto b_start = b.scheduled.count - b.playout_offset.count;
        if (b_start - a_end < 5'000'000 || b_start < last_arrival - 8'000'000'000LL) { continue; }
        const auto t = a_end + (b_start - a_end) / 2;
        const auto expected = a.playout_end_offset.count
            + (b.playout_offset.count - a.playout_end_offset.count) * (t - a_end) / (b_start - a_end);
        replay.gaps.push_back({audio.playout_offset_at(aa::core::Nanoseconds{t}).count(), expected,
                               (b.playout_offset.count - a.playout_end_offset.count) * 1000 / (b_start - a_end)});
    }
    // P2-4: a new session drops the old session's offset; the hold falls back.
    audio.begin_session();
    replay.offset_after_reset = audio.playout_offset_at(aa::core::Nanoseconds{last_arrival});
    replay.hold_after_reset = media.video.presentation_delay_for(aa::core::Nanoseconds{last_arrival});
    audio.stop();
    if (auto finished = audio.finish(); !finished && !replay.failure) {
        replay.failure = finished.error().code();
    }
    if (audio.final_report()) { replay.stats = audio.final_report()->stats; }
    replay.media_timing = telemetry.of("media_timing");
    replay.underruns = telemetry.of("audio_playout_underrun");
    replay.anchors = telemetry.of("audio_playout_anchor");
    replay.completions = *completions;
    return replay;
}

constexpr std::array<std::int64_t, 27> kWindow9RoundTripMs{
    16, 13, 37, 29, 38, 42, 39, 28, 9, 12, 9, 9, 13, 38, 36, 39, 32, 14, 9, 11,
    11, 12, 26, 33, 57, 83, 35,
};

// Window 9's recorded round trips (and the 12:83 ms second that ended the live
// session `late`), then a synthetic 150 ms x 2 s spike: through the production
// wireless policy nothing is trimmed, nothing is silence and nothing fails.
TEST(WirelessAudioWorker, PlayoutReplaysWindow9AndASpikeLosslesslyThroughTheWorker) {
    const auto window9 = replay_through_worker(27, [](std::int64_t second) {
        return kWindow9RoundTripMs.at(static_cast<std::size_t>(std::clamp<std::int64_t>(second, 0, 26)));
    });
    ASSERT_FALSE(window9.failure) << aa::to_string(*window9.failure);
    EXPECT_GT(window9.chunks, 600U);
    EXPECT_EQ(window9.stats.frames_decoded, window9.chunks * 2048U);
    EXPECT_EQ(window9.stats.frames_trimmed, 0U);
    EXPECT_EQ(window9.stats.frames_silence, 0U);
    EXPECT_EQ(window9.stats.playout_underruns, 0U);
    EXPECT_EQ(window9.stats.frames_retained, window9.stats.frames_decoded);
    EXPECT_TRUE(window9.underruns.empty());
    // media_timing shows the buffer to the next live window.
    ASSERT_FALSE(window9.media_timing.empty());
    const auto& timing = window9.media_timing.back();
    EXPECT_NE(timing.find("\"frames_trimmed\":0,"), std::string::npos) << timing;
    EXPECT_NE(timing.find("\"frames_silence\":0,"), std::string::npos) << timing;
    EXPECT_NE(timing.find("\"frames_skipped\":0,"), std::string::npos) << timing;
    EXPECT_NE(timing.find("\"playout_target_ms\":120,"), std::string::npos) << timing;
    EXPECT_NE(timing.find("\"playout_underruns\":0,"), std::string::npos) << timing;
    EXPECT_NE(timing.find("\"credit_window\":4,"), std::string::npos) << timing;
    EXPECT_NE(timing.find("\"max_arrival_burst\":"), std::string::npos) << timing;
    EXPECT_NE(timing.find("\"playout_depth_ms\":"), std::string::npos) << timing;

    const auto spike = replay_through_worker(20, [](std::int64_t second) {
        return second >= 8 && second < 10 ? std::int64_t{150} : std::int64_t{20};
    });
    ASSERT_FALSE(spike.failure) << aa::to_string(*spike.failure);
    EXPECT_EQ(spike.stats.frames_trimmed, 0U);
    EXPECT_EQ(spike.stats.frames_silence, 0U);
    EXPECT_EQ(spike.stats.playout_underruns, 0U);
}

// A drought longer than the buffer underruns once, is logged, and the session
// keeps playing instead of failing `late`.
TEST(WirelessAudioWorker, PlayoutDroughtBeyondTheBufferUnderrunsAndContinues) {
    const auto replay = replay_through_worker(20, [](std::int64_t second) {
        return second == 8 ? std::int64_t{800} : std::int64_t{20};
    });
    ASSERT_FALSE(replay.failure) << aa::to_string(*replay.failure);
    EXPECT_GE(replay.stats.playout_underruns, 1U);
    EXPECT_GT(replay.stats.frames_skipped, 0U);
    ASSERT_FALSE(replay.underruns.empty());
    EXPECT_NE(replay.underruns.front().find("{\"event\":\"audio_playout_underrun\",\"underruns_lifetime\":1,"),
              std::string::npos) << replay.underruns.front();
    // Chunks kept arriving and playing after the underrun.
    EXPECT_GT(replay.stats.frames_retained, replay.stats.frames_decoded * 9 / 10);
}

// A/V alignment: the wireless receiver holds video for the audio playout depth
// by default, so both streams leave the receiver the same time after arrival.
TEST(WirelessAudioWorker, VideoDelayMatchesTheAudioPlayoutDepthByDefault) {
    // Before audio anchors, "match" holds video for the configured depth; the
    // runtime then follows the audio's playout offset (see the burst test).
    EXPECT_EQ(wr::video_delay(std::nullopt, 120), std::chrono::milliseconds{120});
    EXPECT_EQ(wr::video_delay(std::int64_t{0}, 120), std::chrono::milliseconds{0});
    EXPECT_EQ(wr::video_delay(std::nullopt, 0), std::chrono::milliseconds{0});
    const auto config = wr::audio_config();
    EXPECT_TRUE(config.playout.enabled);
    EXPECT_EQ(config.playout.depth, audio::kWirelessPlayoutDepth);
    EXPECT_EQ(wr::video_delay(std::nullopt, audio::kWirelessPlayoutDepth.count / 1'000'000),
              std::chrono::milliseconds{120});
    // Zero restores the arrival model.
    EXPECT_FALSE(wr::audio_config(audio::SinkBackend::pulse, aa::core::Nanoseconds{0},
                                  aa::core::Nanoseconds{0}).playout.enabled);
}


// DeepSeek F1 through the worker: the phone uses its credit window for real
// (groups of 3 arrive together and are held unacknowledged at once), and the
// production wireless policy still trims nothing and underruns never.
TEST(WirelessAudioWorker, PlayoutCoalescedBurstsThroughTheWorkerAreNotTrimmed) {
    const auto replay = replay_through_worker(20, [](std::int64_t second) {
        return second >= 8 && second < 10 ? std::int64_t{120} : std::int64_t{20};
    }, 3);
    ASSERT_FALSE(replay.failure) << aa::to_string(*replay.failure);
    // N1: the audio really plays two chunk periods later than the 120 ms
    // buffer (the anchor was a group's late first chunk), and the wireless
    // video hold follows that delay instead of staying at 120 ms.
    EXPECT_NEAR(static_cast<double>(replay.playout_offset), 120e6 + 2 * static_cast<double>(kChunkNs), 2e6);
    EXPECT_EQ(replay.offset_after_reset.count(), 0);
    EXPECT_EQ(replay.hold_after_reset, std::chrono::milliseconds{120});
    EXPECT_GE(replay.max_unacked_held, 3U);
    EXPECT_GE(replay.stats.max_arrival_burst, 3U);
    EXPECT_EQ(replay.stats.frames_trimmed, 0U);
    EXPECT_EQ(replay.stats.frames_silence, 0U);
    EXPECT_EQ(replay.stats.playout_underruns, 0U);
    ASSERT_FALSE(replay.media_timing.empty());
    EXPECT_NE(replay.media_timing.back().find("\"max_unacked_held\":3,"), std::string::npos)
        << replay.media_timing.back();
}

// DeepSeek F3: every re-anchor that skips time is logged with its size, so a
// huge forward jump is visible; a resume after a pause is still intended.
TEST(WirelessAudioWorker, PlayoutResumeSkipsAreLoggedWithTheirSize) {
    const auto replay = replay_through_worker(12, [](std::int64_t second) {
        return second == 5 ? std::int64_t{4000} : std::int64_t{20};
    });
    ASSERT_FALSE(replay.failure) << aa::to_string(*replay.failure);
    EXPECT_GE(replay.stats.playout_anchors, 2U);
    ASSERT_GE(replay.anchors.size(), 2U);
    EXPECT_NE(replay.anchors.front().find("{\"event\":\"audio_playout_anchor\",\"reason\":\"start\",\"skipped_ms\":0,"),
              std::string::npos) << replay.anchors.front();
    bool resumed = false;
    for (const auto& line : replay.anchors) {
        if (line.find("\"reason\":\"resume\"") == std::string::npos) { continue; }
        resumed = true;
        // N3: the record carries the skip's real size (a ~2 s hole here).
        const std::regex skipped{R"("skipped_ms":([0-9.]+))"};
        std::smatch match;
        ASSERT_TRUE(std::regex_search(line, match, skipped)) << line;
        EXPECT_GT(std::stod(match[1].str()), 1000.0) << line;
    }
    EXPECT_TRUE(resumed);
}

// Neon P1-2: with a credit window the phone holds all N permits. While the
// sink is blocked it may send every one of them; none may be refused (a
// refusal is ipc_queue_full, which ends the session). Saturate guidance
// (6400 B = one 200 ms envelope), media bytes (19200 B envelopes) and the
// media slot cap (small chunks) with a phone that sends exactly what its
// permits allow and counts the acks it gets back.
TEST(WirelessAudioWorker, CreditWindowSaturationWithABlockedSinkNeverRefusesALegalChunk) {
    struct Case final {
        proto::ChannelRole role;
        std::size_t bytes;
        const char* label;
    };
    for (const auto& c : {Case{proto::ChannelRole::guidance_audio, 6400, "guidance"},
                          Case{proto::ChannelRole::media_audio, 19200, "media bytes"},
                          Case{proto::ChannelRole::media_audio, 400, "media slots"}}) {
        auto gate = std::make_shared<SinkGate>();
        gate->block_first_write = true;
        audio::AudioRenderWorker audio{quiet_wireless_config(), {}, fake_router(gate),
                                       audio::AudioRenderWorker::monotonic_now,
                                       audio::AudioRenderWorker::kDefaultAckReleaseDrainTimeout,
                                       audio::AudioCreditPolicy{audio::kWirelessCreditWindow}};
        ASSERT_TRUE(audio.ready());
        const std::vector<std::byte> payload(c.bytes, std::byte{0x10});
        std::uint64_t permits = audio::kWirelessCreditWindow;
        std::uint64_t sent = 0;
        bool refused = false;
        for (std::uint64_t k = 0; k < 64 && permits > 0; ++k) {
            const proto::MessageHeader header{c.role, k, aa::core::Nanoseconds{1'000'000'000LL + static_cast<std::int64_t>(k) * 1'000'000}};
            const auto submitted = audio.submit({header, payload}, {c.role, 0, k + 1, 1});
            if (!submitted) { refused = true; break; }
            ++sent;
            --permits;
            if (k == 0) { ASSERT_TRUE(gate->wait_entered(1)); }
            while (audio.take_completion()) { ++permits; }
        }
        EXPECT_FALSE(refused) << c.label << ": legal chunk " << sent + 1 << " refused";
        // The phone used its whole window before it stalled on withheld credit.
        EXPECT_GE(sent, audio::kWirelessCreditWindow) << c.label;
        gate->release();
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        audio.stop();
        EXPECT_TRUE(audio.finish()) << c.label;
    }
}


// R3 / Neon P2-5: the hold must be the offset of the audio the frame pairs
// with (the write whose content interval holds the frame's timestamp), also
// while the offset moves: the phone changes from single chunks to groups of 4
// mid-run, so the offset climbs by three chunk periods and is absorbed at half
// rate. Sampling the newest write instead held frames for later audio.
TEST(WirelessAudioWorker, VideoHoldIsTheOffsetOfTheAudioItPairsWithWhileTheOffsetMoves) {
    const auto replay = replay_through_worker(16, [](std::int64_t) { return std::int64_t{20}; }, 1, 4, 6);
    ASSERT_FALSE(replay.failure) << aa::to_string(*replay.failure);
    std::size_t compared = 0;
    std::int64_t worst = 0;
    std::int64_t lowest = INT64_MAX;
    std::int64_t highest = 0;
    for (const auto& [content, hold] : replay.holds) {
        const auto reported = reported_offset_at(replay.completions, content);
        if (!reported) { continue; }
        ++compared;
        worst = std::max(worst, std::abs(hold.count() - *reported));
        lowest = std::min(lowest, *reported);
        highest = std::max(highest, *reported);
    }
    EXPECT_GT(compared, 150U);
    EXPECT_LE(worst, 2'000'000) << "hold differs from the paired audio's reported offset";
    // The offset really moved (by three chunk periods).
    EXPECT_GT(highest - lowest, 100'000'000);
}


// Neon P1 #1 (round 4): across an output gap (an underrun's skip and refill)
// the router may lower the offset by half the gap at once. The video lookup
// returned the old offset flat across the gap, so frames arriving in it were
// held for it and the down-slew floor kept the first frames after the resume
// up to ~160 ms too long. It now draws the router's own line across the gap
// (slope >= -1/3 of content time, which is what half the output gap is).
TEST(WirelessAudioWorker, PlayoutOffsetLookupFollowsTheRouterAcrossGaps) {
    const auto replay = replay_through_worker(16, [](std::int64_t second) {
        return second == 10 ? std::int64_t{800} : std::int64_t{20};
    }, 3);
    ASSERT_FALSE(replay.failure) << aa::to_string(*replay.failure);
    ASSERT_GE(replay.stats.playout_underruns, 1U);
    ASSERT_FALSE(replay.gaps.empty());
    bool fell = false;
    for (const auto& gap : replay.gaps) {
        EXPECT_NEAR(static_cast<double>(gap.lookup), static_cast<double>(gap.expected), 1e6);
        EXPECT_GE(gap.slope_milli, -334);
        fell = fell || gap.slope_milli < 0;
    }
    EXPECT_TRUE(fell) << "no gap lowered the offset";
}

// Neon P2 #4 (round 4): the history the video hold reads is dropped exactly
// when no stream plays on the playout clock any more (as the router drops its
// own slew state), not by channel: a guidance stream playing alone and
// stopping leaves no stale offset, and media stopping while guidance plays
// keeps guidance's.
TEST(WirelessAudioWorker, PlayoutHistoryClearsWhenNoStreamIsAnchored) {
    auto gate = std::make_shared<SinkGate>();
    audio::AudioRenderWorker audio{quiet_wireless_config(), {}, fake_router(gate)};
    ASSERT_TRUE(audio.ready());
    const auto guidance = proto::ChannelRole::guidance_audio;
    const auto media = proto::ChannelRole::media_audio;
    const std::vector<std::byte> voice(6400, std::byte{0x10});
    const aa::core::Nanoseconds t0{1'000'000'000};
    ASSERT_TRUE(audio.submit({{guidance, 1, t0}, voice}, {guidance, 0, 1, 1}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_GT(audio.playout_offset_at(t0).count(), 0);
    audio.invalidate(guidance, 1); // guidance alone stops
    EXPECT_EQ(audio.playout_offset_at(t0).count(), 0);

    const aa::core::Nanoseconds t1{3'000'000'000};
    ASSERT_TRUE(audio.submit({{guidance, 2, t1}, voice}, {guidance, 1, 2, 1}));
    ASSERT_TRUE(audio.submit({{media, 3, t1}, pcm_chunk_2048()}, {media, 0, 3, 1}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_GT(audio.playout_offset_at(t1).count(), 0);
    audio.invalidate(media, 1); // media stops; guidance still plays
    EXPECT_GT(audio.playout_offset_at(t1).count(), 0);
    audio.stop();
    EXPECT_TRUE(audio.finish());
}

} // namespace
