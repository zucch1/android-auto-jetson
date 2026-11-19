// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../transport/UsbAoaFakes.hpp"
#include "../trust/fakes.hpp"
#include "../trust/syscall_faults.hpp"
#include "../../src/transport/usb_adapter/UsbAoaEndpoint.hpp"
#include <aa/app/WiredLink.hpp>
#include <aa/app/WiredPhoneControl.hpp>
#include <aa/transport/UsbByteTransport.hpp>

#include <gtest/gtest.h>
#include <condition_variable>
#include <future>
#include <thread>

namespace {
namespace app = aa::app;
namespace ss = aa::session;
namespace trust = aa::trust;
namespace faults = trust::test::faults;
namespace usb = aa::test::usb_aoa;
using aa::ErrorCode;
using namespace std::chrono_literals;

class Watchdog {
  public:
    explicit Watchdog(app::WiredSession& session) : thread_([this, &session](const std::stop_token& stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, 3s, [] { return false; });
        if (!stop.stop_requested()) {
            fired.store(true);
            session.request_stop();
        }
    }) {}
    std::atomic<bool> fired{false};
  private:
    std::jthread thread_;
};

bool wait_reads(const std::shared_ptr<usb::PipeState>& pipe, int count) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (pipe->reads_started.load() < count && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(1ms);
    }
    return pipe->reads_started.load() >= count;
}

bool closed(const std::shared_ptr<usb::PipeState>& pipe) {
    const std::lock_guard lock(pipe->mutex);
    return pipe->closed;
}

class WiredE2eRevoke : public testing::Test {
  protected:
    trust::test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ss::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    app::WiredSession session{{store, clock, diagnostics}, {}};
    app::WiredPhoneControl control{store, session};
    std::unique_ptr<aa::transport::UsbByteTransport> transport;
    usb::FakeBulkPipe* pipe{};
    std::shared_ptr<usb::PipeState> observed;
    trust::WiredIdentity wired;
    trust::PhoneIdentity phone;
    aa::core::PhoneId id;
    std::vector<std::thread::id> admissions;

    void SetUp() override {
        ASSERT_TRUE(store.load().has_value());
        auto factory = std::make_unique<usb::FakeLinkFactory>();
        auto* driver = factory.get();
        wired = aa::transport::usb_adapter::build_wired_identity(
            factory->phone, factory->accessory, "1-2.1");
        phone = app::wired_phone_identity(wired).value();
        auto approved = store.approve(trust::TransportIdentity{wired});
        ASSERT_TRUE(approved.has_value());
        id = approved.value();
        aa::transport::UsbDeviceSelector selector;
        selector.port_context = "1-2.1";
        auto endpoint = aa::transport::usb_adapter::UsbAoaEndpoint::make(
            std::move(factory), selector, {}, [this](const trust::WiredIdentity& candidate) {
                admissions.push_back(std::this_thread::get_id());
                return aa::transport::admit_via(store)(candidate);
            });
        ASSERT_TRUE(endpoint.has_value());
        auto framed = aa::transport::UsbByteTransport::over_endpoint(std::move(endpoint.value()));
        ASSERT_TRUE(framed.has_value());
        transport = std::move(framed.value());
        pipe = driver->pipe();
        observed = pipe->state();
        session.attach_transport(transport.get());
        ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
        ASSERT_TRUE(session.phone_discovered(phone).has_value());
        ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
        ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded).has_value());
        control.bind_owner();
        control.bind_phone(phone);
        transport->set_io_checkpoint([this] { control.checkpoint(); });
    }

    void TearDown() override {
        (void)control.finish();
        transport->set_io_checkpoint({});
        control.close();
        faults::set_observer({});
        faults::disarm();
    }

    std::vector<std::byte> ping() {
        return wiredtest::control_frame(aa::protocol::encode(aa::protocol::PingRequest{1, {}}).value());
    }

    aa::core::Result<void> forget_blocked(bool partial = false,
                                         faults::Fault fault = faults::Fault::none) {
        const auto frame = ping();
        if (partial) {
            pipe->push_in({frame.begin(), frame.begin() + 2});
        }
        faults::arm(fault);
        Watchdog watchdog{session};
        aa::core::Result<void> reply{aa::Error{ErrorCode::internal}};
        bool after_close = false;
        std::thread requester([&] {
            EXPECT_TRUE(wait_reads(observed, partial ? 2 : 1));
            reply = control.forget(id);
            after_close = closed(observed);
        });
        const auto polled = session.poll_transport();
        control.checkpoint();
        EXPECT_TRUE(control.finish());
        requester.join();
        faults::disarm();
        EXPECT_FALSE(watchdog.fired.load()) << "watchdog teardown is not revocation";
        EXPECT_FALSE(polled.has_value());
        EXPECT_TRUE(session.token().stop_requested());
        EXPECT_EQ(session.state(), ss::State::failed);
        EXPECT_TRUE(session.failure().has_value());
        if (session.failure()) {
            EXPECT_EQ(session.failure()->code(), ErrorCode::session_unknown_phone);
        }
        EXPECT_TRUE(after_close);
        EXPECT_TRUE(closed(observed));
        EXPECT_EQ(session.pings_answered(), 0U);
        return reply;
    }

    void reload_unapproved() {
        EXPECT_EQ(store.lookup(phone), trust::Decision::unknown);
        trust::ApprovedPhoneStore reloaded{dir.file()};
        ASSERT_TRUE(reloaded.load().has_value());
        EXPECT_EQ(reloaded.lookup(phone), trust::Decision::unknown);
    }
};

TEST_F(WiredE2eRevoke, RevokeDisconnectsAndBlocksReconnectUntilReapproved) {
    ASSERT_TRUE(forget_blocked().has_value());
    reload_unapproved();
    EXPECT_FALSE(aa::transport::admit_via(store)(wired));
    EXPECT_FALSE(session.apply(ss::Event::reconnect).has_value());
    EXPECT_EQ(session.state(), ss::State::failed);
    ASSERT_TRUE(session.apply(ss::Event::reset).has_value());
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    EXPECT_FALSE(session.phone_discovered(phone).has_value());
    EXPECT_NE(session.state(), ss::State::active);
    ASSERT_TRUE(store.approve(trust::TransportIdentity{wired}).has_value());
    EXPECT_TRUE(aa::transport::admit_via(store)(wired));
    wiredtest::ScriptedTransport fresh;
    session.attach_transport(&fresh);
    ASSERT_TRUE(session.phone_discovered(phone).has_value());
    EXPECT_EQ(session.state(), ss::State::connecting);
    EXPECT_TRUE(fresh.is_open());
    session.request_stop();
    session.tick();
    ASSERT_TRUE(session.apply(ss::Event::cleanup_finished).has_value());
}

TEST_F(WiredE2eRevoke, ForgetQuietReceiveCancelsWithoutLinkLoss) {
    EXPECT_TRUE(forget_blocked().has_value());
    reload_unapproved();
}

TEST_F(WiredE2eRevoke, ForgetPartialReceiveDropsFrameWithoutDelivery) {
    EXPECT_TRUE(forget_blocked(true).has_value());
    EXPECT_EQ(observed->out_size(), 0U);
}

TEST_F(WiredE2eRevoke, CommittedNotDurableForgetStillDisconnects) {
    const auto result = forget_blocked(false, faults::Fault::dir_fsync);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::trust_store_not_durable);
    reload_unapproved();
}

TEST_F(WiredE2eRevoke, PreCommitForgetFailurePreservesApprovalAndSession) {
    faults::arm(faults::Fault::temp_fsync);
    Watchdog watchdog{session};
    aa::core::Result<void> reply;
    std::thread requester([&] {
        EXPECT_TRUE(wait_reads(observed, 1));
        reply = control.forget(id);
        EXPECT_FALSE(closed(observed));
        pipe->push_in(ping());
    });
    const auto got = session.poll_transport();
    requester.join();
    faults::disarm();
    EXPECT_FALSE(watchdog.fired.load());
    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().code(), ErrorCode::trust_store_io);
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got.value());
    EXPECT_EQ(session.pings_answered(), 1U);
    EXPECT_FALSE(session.token().stop_requested());
    EXPECT_FALSE(control.finish());
    EXPECT_FALSE(closed(observed));
    EXPECT_EQ(session.state(), ss::State::active);
    EXPECT_EQ(store.lookup(phone), trust::Decision::approved);
}

TEST_F(WiredE2eRevoke, OtherPhoneForgetDoesNotCancelCurrentSession) {
    auto other = wired;
    other.usb_serial = "OTHER";
    const auto approved = store.approve(trust::TransportIdentity{other});
    ASSERT_TRUE(approved.has_value());
    const auto frame = ping();
    pipe->push_in({frame.begin(), frame.begin() + 2});
    Watchdog watchdog{session};
    aa::core::Result<void> reply;
    std::thread requester([&] {
        EXPECT_TRUE(wait_reads(observed, 2));
        reply = control.forget(approved.value());
        EXPECT_FALSE(closed(observed));
        pipe->push_in({frame.begin() + 2, frame.end()});
    });
    const auto got = session.poll_transport();
    requester.join();
    EXPECT_FALSE(watchdog.fired.load());
    EXPECT_TRUE(reply.has_value());
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got.value());
    EXPECT_EQ(session.pings_answered(), 1U);
    EXPECT_FALSE(session.token().stop_requested());
    EXPECT_FALSE(control.finish());
    EXPECT_FALSE(closed(observed));
    EXPECT_EQ(session.state(), ss::State::active);
    EXPECT_EQ(store.lookup(phone), trust::Decision::approved);
    EXPECT_EQ(store.lookup(app::wired_phone_identity(other).value()), trust::Decision::unknown);
}

// #90 review: the durable write (temp file, fsync, rename, directory fsync)
// runs off the session owner and off the D-Bus requester; the in-memory
// revocation and admission stay on the owner.
TEST_F(WiredE2eRevoke, ForgetDurableWriteRunsOffOwnerWhileMutationAndAdmissionStayOnOwner) {
    const auto owner = std::this_thread::get_id();
    std::vector<std::thread::id> mutations;
    faults::set_observer([&] { mutations.push_back(std::this_thread::get_id()); });
    std::thread::id requesting;
    Watchdog watchdog{session};
    aa::core::Result<void> reply;
    std::thread requester([&] {
        requesting = std::this_thread::get_id();
        EXPECT_TRUE(wait_reads(observed, 1));
        reply = control.forget(id);
    });
    EXPECT_FALSE(session.poll_transport().has_value());
    control.checkpoint();
    EXPECT_TRUE(control.finish());
    requester.join();
    faults::set_observer({});
    EXPECT_FALSE(watchdog.fired.load());
    EXPECT_TRUE(reply.has_value());
    ASSERT_FALSE(mutations.empty());
    for (const auto thread : mutations) {
        EXPECT_NE(thread, owner);
        EXPECT_NE(thread, requesting);
    }
    EXPECT_NE(store.lookup(phone), trust::Decision::approved);
    ASSERT_FALSE(admissions.empty());
    for (const auto thread : admissions) {
        EXPECT_EQ(thread, owner);
        EXPECT_NE(thread, requesting);
    }
    EXPECT_FALSE(aa::transport::admit_via(store)(wired));
}

struct StalledPersist {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false};
    bool released{false};
    std::thread::id thread{};
    aa::core::Result<void> operator()(const trust::PreparedForget& prepared) {
        {
            std::unique_lock lock(mutex);
            entered = true;
            thread = std::this_thread::get_id();
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }
        return trust::ApprovedPhoneStore::write_prepared(prepared);
    }
    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    bool wait_entered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return entered; });
    }
};

// #90 review P1: revoking an approved spare phone during projection must not
// stall the session while the trust store's fsync is stuck.
TEST_F(WiredE2eRevoke, StalledForgetPersistenceNeverBlocksTheActiveSession) {
    auto other = wired;
    other.usb_serial = "SPARE";
    const auto spare = store.approve(trust::TransportIdentity{other});
    ASSERT_TRUE(spare.has_value());
    auto stall = std::make_shared<StalledPersist>();
    app::WiredPhoneControl stalled{store, session,
                                   [stall](const trust::PreparedForget& p) { return (*stall)(p); }};
    stalled.bind_owner();
    stalled.bind_phone(phone);
    auto reply = std::async(std::launch::async, [&] { return stalled.forget(spare.value()); });
    // When: the owner takes the request and the durable write stalls.
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!stall->entered && std::chrono::steady_clock::now() < until) {
        const auto t0 = std::chrono::steady_clock::now();
        stalled.checkpoint();
        EXPECT_LT(std::chrono::steady_clock::now() - t0, 50ms) << "checkpoint waited on disk";
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(stall->wait_entered());
    EXPECT_NE(stall->thread, std::this_thread::get_id());
    // Then: the owner keeps servicing the session (a ping is answered) and
    // checkpoints stay immediate while the fsync is stuck.
    pipe->push_in(ping());
    const auto t_poll = std::chrono::steady_clock::now();
    const auto polled = session.poll_transport();
    ASSERT_TRUE(polled.has_value());
    EXPECT_TRUE(polled.value());
    EXPECT_EQ(session.pings_answered(), 1U);
    for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        stalled.checkpoint();
        EXPECT_LT(std::chrono::steady_clock::now() - t0, 50ms);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - t_poll, 1s);
    EXPECT_EQ(reply.wait_for(0ms), std::future_status::timeout);
    // Precommit: trust is unchanged until the write lands.
    EXPECT_EQ(store.lookup(app::wired_phone_identity(other).value()), trust::Decision::approved);
    // When: the disk recovers, a later checkpoint applies and answers.
    stall->release();
    while (reply.wait_for(1ms) != std::future_status::ready) {
        stalled.checkpoint();
    }
    EXPECT_TRUE(reply.get().has_value());
    EXPECT_EQ(store.lookup(app::wired_phone_identity(other).value()), trust::Decision::unknown);
    EXPECT_EQ(store.lookup(phone), trust::Decision::approved);
    EXPECT_FALSE(session.token().stop_requested());
    EXPECT_FALSE(stalled.finish());
    stalled.close();
}

TEST_F(WiredE2eRevoke, ShutdownBoundsAStalledForgetAndReportsTimeout) {
    auto other = wired;
    other.usb_serial = "SPARE";
    const auto spare = store.approve(trust::TransportIdentity{other});
    ASSERT_TRUE(spare.has_value());
    auto stall = std::make_shared<StalledPersist>();
    {
        app::WiredPhoneControl stalled{store, session,
                                       [stall](const trust::PreparedForget& p) { return (*stall)(p); }};
        stalled.bind_owner();
        stalled.bind_phone(phone);
        auto reply = std::async(std::launch::async, [&] { return stalled.forget(spare.value()); });
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!stall->entered && std::chrono::steady_clock::now() < until) {
            stalled.checkpoint();
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_TRUE(stall->wait_entered());
        // When: the owner shuts down while the fsync never returns.
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_FALSE(stalled.finish());
        stalled.close();
        const auto waited = std::chrono::steady_clock::now() - t0;
        // Then: shutdown is bounded and the caller learns the outcome is unknown.
        EXPECT_GE(waited, app::WiredPhoneControl::kShutdownPersistWait - 50ms);
        EXPECT_LT(waited, app::WiredPhoneControl::kShutdownPersistWait + 1s);
        ASSERT_EQ(reply.wait_for(1s), std::future_status::ready);
        const auto result = reply.get();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::timeout);
        EXPECT_EQ(store.lookup(app::wired_phone_identity(other).value()), trust::Decision::approved);
    }
    stall->release(); // let the abandoned writer finish; it touches only its copies
}

TEST_F(WiredE2eRevoke, PendingForgetCompletesDuringOwnerShutdown) {
    auto first = std::async(std::launch::async, [&] { return control.forget(id); });
    auto second = std::async(std::launch::async, [&] { return control.forget(id); });
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (first.wait_for(0ms) != std::future_status::ready &&
           second.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(1ms);
    }
    const bool observed_full = first.wait_for(0ms) == std::future_status::ready ||
                               second.wait_for(0ms) == std::future_status::ready;
    session.request_stop();
    session.tick();
    ASSERT_TRUE(session.apply(ss::Event::cleanup_finished).has_value());
    control.close();
    EXPECT_TRUE(observed_full);
    ASSERT_EQ(first.wait_for(1s), std::future_status::ready);
    ASSERT_EQ(second.wait_for(1s), std::future_status::ready);
    const auto a = first.get();
    const auto b = second.get();
    ASSERT_FALSE(a.has_value());
    ASSERT_FALSE(b.has_value());
    EXPECT_TRUE((a.error().code() == ErrorCode::ipc_queue_full &&
                 b.error().code() == ErrorCode::cancelled) ||
                (b.error().code() == ErrorCode::ipc_queue_full &&
                 a.error().code() == ErrorCode::cancelled));
    const auto late = control.forget(id);
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error().code(), ErrorCode::cancelled);
    control.checkpoint();
    EXPECT_EQ(store.lookup(phone), trust::Decision::approved);
}

} // namespace
