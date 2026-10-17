// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../transport/UsbAoaFakes.hpp"
#include "../../src/transport/usb_adapter/UsbAoaEndpoint.hpp"
#include <aa/transport/UsbByteTransport.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace {
using namespace std::chrono_literals;
namespace ua = aa::transport::usb_adapter;
namespace fx = aa::test::usb_aoa;

struct DeadlineFixture {
    aa::session::test::MapTrustStore trust;
    aa::session::SteadyClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    std::shared_ptr<fx::LinkState> observations = std::make_shared<fx::LinkState>();
    fx::FakeBulkPipe *pipe{};
    std::unique_ptr<aa::transport::UsbByteTransport> transport;
    std::unique_ptr<aa::app::WiredSession> session;

    explicit DeadlineFixture(std::int64_t budget_ms = 100) {
        const aa::trust::PhoneIdentity phone{"approved"};
        trust.set(phone, aa::trust::Decision::approved);
        auto factory = std::make_unique<fx::FakeLinkFactory>(observations);
        auto *driving = factory.get();
        auto endpoint = ua::UsbAoaEndpoint::make(std::move(factory), {"1-2.1"}, {},
                                                [](const auto &) { return true; });
        EXPECT_TRUE(endpoint.value()->open().has_value());
        pipe = driving->pipe();
        transport = std::move(aa::transport::UsbByteTransport::over_endpoint(
                                  std::move(endpoint.value())).value());
        aa::app::WiredSessionConfig config;
        config.lifecycle.phases.negotiate = aa::core::Milliseconds{budget_ms};
        session = std::make_unique<aa::app::WiredSession>(
            aa::app::WiredSessionDeps{trust, clock, diagnostics}, config);
        session->attach_transport(transport.get());
        EXPECT_TRUE(session->apply(aa::session::Event::start_discovery).has_value());
        EXPECT_TRUE(session->phone_discovered(phone).has_value());
        EXPECT_TRUE(session->apply(aa::session::Event::transport_ready).has_value());
    }

    std::jthread watchdog() {
        return std::jthread([this](const std::stop_token &stop) {
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock lock(mutex);
            cv.wait_for(lock, stop, 700ms, [] { return false; });
            if (!stop.stop_requested()) {
                session->request_stop();
            }
        });
    }
};

TEST(WiredDeadline, VersionRequestStallThenAcceptSendsExactlyOneFrame) {
    DeadlineFixture fixture{1000};
    fixture.pipe->zero_progress_writes = 4;
    const auto result = fixture.session->send_version_request(1, 6);
    ASSERT_TRUE(result.has_value()) << aa::to_string(result.error().code());
    const auto expected = wiredtest::control_frame(
        aa::protocol::encode(aa::protocol::VersionRequest{1, 6}).value());
    EXPECT_EQ(fixture.observations->pipe_state()->out_bytes(), expected);
}

TEST(WiredDeadline, NeverAcceptingVersionRequestFailsWithNegotiationTimeout) {
    DeadlineFixture fixture;
    fixture.pipe->block_writes = true;
    auto watchdog = fixture.watchdog();
    const auto began = std::chrono::steady_clock::now();
    const auto result = fixture.session->send_version_request(1, 6);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::timeout);
    EXPECT_EQ(fixture.session->state(), aa::session::State::failed);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 500ms);
    EXPECT_EQ(fixture.observations->pipe_state()->out_size(), 0U);
}

TEST(WiredDeadline, PendingReadCannotOutliveNegotiationBudget) {
    DeadlineFixture fixture;
    auto watchdog = fixture.watchdog();
    const auto began = std::chrono::steady_clock::now();
    const auto result = fixture.session->poll_transport();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::timeout);
    EXPECT_EQ(fixture.session->state(), aa::session::State::failed);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 500ms);
}

TEST(WiredDeadline, DefaultNegotiationBudgetExpires) {
    aa::session::test::MapTrustStore trust;
    const aa::trust::PhoneIdentity phone{"approved"};
    trust.set(phone, aa::trust::Decision::approved);
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.apply(aa::session::Event::start_discovery).has_value());
    ASSERT_TRUE(session.phone_discovered(phone).has_value());
    ASSERT_TRUE(session.apply(aa::session::Event::transport_ready).has_value());
    clock.advance(aa::core::Milliseconds{16'000});
    session.tick();
    EXPECT_EQ(session.state(), aa::session::State::failed);
    ASSERT_TRUE(session.failure().has_value());
    EXPECT_EQ(session.failure()->code(), aa::ErrorCode::timeout);
}

TEST(WiredDeadline, PartialProgressThenStallNeverReplaysAcceptedBytes) {
    DeadlineFixture fixture{1000};
    fixture.pipe->max_write_bytes = 3;
    fixture.pipe->stall_after_first_write = 4;
    ASSERT_TRUE(fixture.session->send_version_request(1, 6).has_value());
    const auto expected = wiredtest::control_frame(
        aa::protocol::encode(aa::protocol::VersionRequest{1, 6}).value());
    EXPECT_EQ(fixture.observations->pipe_state()->out_bytes(), expected);
}

TEST(WiredDeadline, RealErrorAfterPartialProgressRetiresLinkWithoutReplay) {
    DeadlineFixture fixture{1000};
    fixture.pipe->max_write_bytes = 3;
    fixture.pipe->fail_write_after = 3;
    const auto result = fixture.session->send_version_request(1, 6);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_io);
    EXPECT_EQ(fixture.observations->pipe_state()->out_size(), 3U);
    EXPECT_FALSE(fixture.session->send_version_request(1, 6).has_value());
    EXPECT_EQ(fixture.observations->pipe_state()->out_size(), 3U);
}
} // namespace
