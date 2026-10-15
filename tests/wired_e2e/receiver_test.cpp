// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual harness adapters, not copies of their logging logic.
#define main wired_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include)
#undef main

#include "harness.hpp"
#include <gtest/gtest.h>

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
} // namespace
