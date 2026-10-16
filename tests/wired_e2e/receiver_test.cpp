// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual harness adapters, not copies of their logging logic.
#define main wired_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include)
#undef main

#include "harness.hpp"
#include <gtest/gtest.h>
#include <condition_variable>
#include <unistd.h>

namespace {
std::atomic<int> runtime_connects{0};
std::atomic<int> runtime_ciphers{0};
bool runtime_recovers = false;
aa::trust::PhoneIdentity runtime_phone;

class RuntimeTransport final : public aa::transport::Transport {
  public:
    explicit RuntimeTransport(bool drops) : drops_(drops) {
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::VersionResponse{1, 6, 0}).value()));
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::EncapsulatedSsl{{std::byte{1}}}).value()));
        frames_.push_back(wiredtest::open_request_frame(99));
    }
    aa::transport::Kind kind() const noexcept override { return aa::transport::Kind::usb_aoa; }
    aa::core::Result<void> open(aa::core::CancellationToken token) override {
        token_ = token;
        return {};
    }
    aa::core::Result<void> send(std::span<const std::byte>) override { return {}; }
    aa::core::Result<std::vector<std::byte>> receive() override {
        if (next_ < frames_.size()) {
            return frames_[next_++];
        }
        if (drops_) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait(lock, token_, [] { return false; });
        return aa::Error{aa::ErrorCode::cancelled};
    }
    void close() noexcept override {}
  private:
    bool drops_;
    std::size_t next_{};
    aa::core::CancellationToken token_;
    std::vector<std::vector<std::byte>> frames_;
};
}

// Link-time seams keep the real CLI/thread loop under test without USB or audio hardware.
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
                  const aa::trust::TrustStore &, const aa::core::Deadline &)
    asm("__wrap__ZN2aa3app15open_wired_linkERKNS_9transport17UsbDeviceSelectorERKNS1_16AoaAccessoryInfoERKNS_5trust10TrustStoreERKNS_4core8DeadlineE");
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
                  const aa::trust::TrustStore &, const aa::core::Deadline &) {
    const int attempt = ++runtime_connects;
    if (attempt == 1 || (runtime_recovers && attempt >= 4)) {
        return aa::app::WiredLink{std::make_unique<RuntimeTransport>(attempt == 1), runtime_phone};
    }
    return aa::Error{aa::ErrorCode::transport_unavailable};
}

std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &)
    asm("__wrap__ZN2aa9transport28make_client_handshake_cipherERKSt8functionIFbvEE");
std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &) {
    ++runtime_ciphers;
    return std::make_unique<wiredtest::FakeHandshakeCipher>();
}

aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &)
    asm("__wrap__ZN2aa5audio6detail17open_default_sinkERKNS1_15SinkOpenRequestE");
aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &) {
    return {std::make_unique<wiredtest::RecordingObservedSink>(
                std::make_shared<wiredtest::AudioObs>()), {}};
}

namespace {
class RejectAudio final : public aa::audio::detail::ObservedAudioSink {
  public:
    aa::core::Result<void> write(const aa::audio::MixedChunk &) override {
        return aa::Error{aa::ErrorCode::transport_io};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
};

TEST(WiredReceiver, FailedAudioPumpDoesNotEmitDelivery) {
    auto router = aa::audio::detail::open_router_with_sink_for_test(
        wiredtest::pcm_router_config(), std::make_unique<RejectAudio>());
    ASSERT_TRUE(router.has_value());
    RouterSink sink{router.value()};
    const auto pcm = wiredtest::pcm_unit();
    aa::protocol::MessageView message{
        {aa::protocol::ChannelRole::media_audio, 1, {}}, pcm};
    testing::internal::CaptureStdout();
    const auto result = sink.on_message(message);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(output.find("audio_chunks"), std::string::npos)
        << "failed pump emitted delivery evidence: " << output;
}

struct RuntimeResult {
    int code;
    std::string output;
    std::chrono::steady_clock::duration elapsed;
};

RuntimeResult run_runtime(bool recovers) {
    char directory[] = "/tmp/wired-runtime-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore store{path};
    EXPECT_TRUE(store.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    EXPECT_TRUE(store.approve(aa::trust::TransportIdentity{identity}).has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_recovers = recovers;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    g_running.store(true);
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--no-bus", "--store",
                                  path.string(), "--video-socket", (root / "video").string(),
                                  "--input-socket", (root / "input").string(), "--duration-ms",
                                  recovers ? "1000" : "6500"};
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    testing::internal::CaptureStdout();
    const auto began = std::chrono::steady_clock::now();
    const int code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    const auto elapsed = std::chrono::steady_clock::now() - began;
    auto output = testing::internal::GetCapturedStdout();
    std::filesystem::remove_all(root);
    return {code, std::move(output), elapsed};
}

TEST(WiredReceiver, TransientLinkLossRetriesUntilFreshSessionRecovers) {
    const auto result = run_runtime(true);
    EXPECT_EQ(result.code, 0) << result.output;
    EXPECT_GE(runtime_connects.load(), 4) << "reconnect stopped after one immediate attempt";
    EXPECT_EQ(runtime_ciphers.load(), 2);
    const auto active = result.output.find("\"to_state\":\"active\"");
    ASSERT_NE(active, std::string::npos) << result.output;
    EXPECT_NE(result.output.find("\"to_state\":\"active\"", active + 1), std::string::npos)
        << result.output;
}

TEST(WiredReceiver, PermanentLinkLossExitsNonzeroWithinWiredRecoveryBudget) {
    const auto result = run_runtime(false);
    EXPECT_EQ(result.code, 1) << "dead session returned clean success: " << result.output;
    EXPECT_GE(runtime_connects.load(), 3);
    EXPECT_EQ(runtime_ciphers.load(), 1);
    EXPECT_LT(result.elapsed, std::chrono::milliseconds{5500});
}
} // namespace
