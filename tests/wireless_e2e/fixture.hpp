// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/app/WirelessLink.hpp>
#include "../session/Fakes.hpp"
#include <aa/trust/Identity.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace wireless_test {
using namespace std::chrono_literals;
struct Diagnostics final : aa::diagnostics::DiagnosticSink {
    void emit_json(std::string_view) override {}
};
struct RecordingSink final : aa::protocol::MessageSink {
    std::vector<std::vector<std::byte>> messages;
    aa::core::Result<void> on_message(const aa::protocol::MessageView& message) override {
        messages.emplace_back(message.payload.begin(), message.payload.end()); return {};
    }
};
struct Fixture {
    aa::session::test::MapTrustStore trust;
    aa::session::SteadyClock clock;
    Diagnostics sink;
    aa::diagnostics::Diagnostics diagnostics{sink};
    aa::helper::ObservedPeer peer{41, {"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
    aa::trust::PhoneIdentity identity{aa::trust::identity_key(aa::trust::TransportIdentity{
        aa::trust::WirelessIdentity{peer.identity.address, peer.identity.adapter}})};
    aa::helper::ApSnapshot snapshot{{"fixture", "test-password", "12:34:56:78:9a:bd", "127.0.0.1", 0, 1}};
    std::atomic<bool> helper_live{true};
    bool ready_before_allow{};
    RecordingSink video, audio, input;
    aa::app::WirelessLink link{{trust, clock, diagnostics}, config()};
    aa::app::HandoffEligibilityCoordinator coordinator{trust,
        {[this](aa::app::HandoffOperations::Validation request) -> aa::core::Result<aa::app::HandoffObservation> {
             if (!helper_live) return aa::Error{aa::ErrorCode::helper_rejected};
              return aa::app::HandoffObservation{peer, request.credentials_allowed ? std::optional{snapshot} : std::nullopt};
         }, [this](aa::helper::Source, std::uint64_t, bool allow) -> aa::core::Result<void> {
             if (allow) ready_before_allow = link.listening();
             return {};
         }, [](aa::helper::Source, std::uint64_t) -> aa::core::Result<void> { return {}; }},
        {[this](const aa::helper::ApSnapshot& ap) { return link.listen(ap); },
         [this] { link.close_admission(); }}};
    aa::app::WirelessLink::Config config() {
        aa::app::WirelessLink::Config result;
        result.services = {
            {aa::protocol::ServiceKey{99}, aa::protocol::ChannelRole::video,
             aa::protocol::VideoConfiguration{{{800, 480, 30}}}},
            {aa::protocol::ServiceKey{100}, aa::protocol::ChannelRole::media_audio,
             aa::protocol::AudioConfiguration{{{48000, 16, 2}}}},
            {aa::protocol::ServiceKey{104}, aa::protocol::ChannelRole::input,
             aa::protocol::ButtonConfiguration{{66}}}};
        result.sinks = {{aa::protocol::ChannelRole::video, &video},
                        {aa::protocol::ChannelRole::media_audio, &audio},
                        {aa::protocol::ChannelRole::input, &input}};
        return result;
    }
    Fixture() {
        trust.set(identity, aa::trust::Decision::approved);
        auto reservation = aa::transport::TcpListener::listen("127.0.0.1", 0);
        EXPECT_TRUE(reservation);
        if (reservation) snapshot.port = reservation.value()->port();
    }
    ~Fixture() { link.stop(); coordinator.retire(); }
    void start() {
        coordinator.consumer(aa::core::ConsumerId{7});
        coordinator.begin({7, 0});
        const auto until = std::chrono::steady_clock::now() + 500ms;
        while (!coordinator.admitting() && std::chrono::steady_clock::now() < until) {
            coordinator.tick(); std::this_thread::sleep_for(2ms);
        }
        ASSERT_TRUE(coordinator.admitting());
        ASSERT_TRUE(ready_before_allow);
    }
    aa::core::Result<std::unique_ptr<aa::transport::TcpTransport>> connect() const {
        return aa::transport::TcpTransport::connect(snapshot.ip.c_str(), static_cast<std::uint16_t>(snapshot.port));
    }
};
}
