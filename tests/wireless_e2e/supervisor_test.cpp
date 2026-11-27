// SPDX-License-Identifier: GPL-3.0-or-later
// The packaged supervisor loop (#221, #240 review P3-7) driven through its
// ports: a real ControlService, fake wired worker, fake wireless run and a
// scripted USB scan.
#include "../../tools/wireless-receiver/Supervisor.hpp"
#include "../dbus/fakes.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace {

namespace wr = aa::wireless_receiver;
namespace ipc = aa::ipc;
using aa::ErrorCode;
using namespace std::chrono_literals;

constexpr std::uint32_t kUid = 1000;

const aa::trust::WiredIdentity kWiredIdentity{
    "18d1", "4ee1", "wired-serial", "Android", "Android Auto", "wired-accessory", "1-1"};
const aa::trust::TransportIdentity kWirelessA{
    aa::trust::WirelessIdentity{"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
const aa::trust::TransportIdentity kWirelessB{
    aa::trust::WirelessIdentity{"12:34:56:78:9A:BE", "12:34:56:78:9A:BD"}};

wr::ArbitrationPolicy fast_policy() {
    wr::ArbitrationPolicy policy;
    policy.healthy_run = 300ms;
    policy.first_backoff = 10ms;
    policy.max_backoff = 40ms;
    policy.wired_attempts = 3;
    policy.absent_reset = 60ms;
    policy.scan_interval = 0ms;
    return policy;
}

struct Harness final {
    ipc::test::FakePeerCredentials credentials{};
    ipc::test::RecordingEventSink events{};
    ipc::test::SequencePairingIds ids{};
    ipc::test::RecordingPhoneDirectory phones{};
    ipc::ProjectionPolicy policy{};
    ipc::ControlService service{{kUid}, {credentials, events, ids, phones, nullptr, {},
                                         [this] { return worker_running; }, &policy}};
    const ipc::BusName owner{":1.40"};
    const ipc::BusName name{"org.custom.Consumer"};

    std::vector<aa::trust::ApprovedPhone> approved{};
    std::vector<wr::WiredCandidate> attached{};
    bool worker_running{};
    unsigned worker_starts{};
    unsigned worker_stops{};
    unsigned idr_requests{};
    bool spawn_fails{};
    unsigned wireless_runs{};
    unsigned releases{};
    bool process_running{true};
    bool wireless_idle{true};
    std::vector<std::string> reports{};
    // The wireless run: returns its status after `keep` says stop, or at once.
    std::function<int(const wr::WirelessKeep&)> wireless = [](const wr::WirelessKeep&) { return 0; };

    wr::Supervisor supervisor{wr::SupervisorPorts{
        [](aa::core::Milliseconds) -> aa::core::Result<void> { return {}; },
        [this]() -> ipc::ControlService& { return service; },
        policy,
        [this]() -> const std::vector<aa::trust::ApprovedPhone>& { return approved; },
        [this](const std::optional<wr::WiredCandidate>&) { return attached; },
        [this](const wr::WiredCandidate&) {
            if (spawn_fails) return false;
            ++worker_starts;
            worker_running = true;
            return true;
        },
        [this] { return worker_running; },
        [this] {
            if (worker_running) ++worker_stops;
            worker_running = false;
        },
        [this] { ++idr_requests; },
        [this](const wr::WirelessKeep& keep) {
            ++wireless_runs;
            return wireless(keep);
        },
        [this] { return wireless_idle; },
        [this] { ++releases; },
        [this] { return process_running; },
        [this](const char* what, ErrorCode) { reports.emplace_back(what); },
    }, fast_policy()};

    Harness() {
        credentials.add_connection(owner, kUid);
        credentials.set_owner(name, owner);
        policy.check_ready = [] { return aa::core::Result<void>{}; };
    }
    aa::core::ConsumerId start() {
        const auto registered = service.register_consumer(owner, name);
        EXPECT_TRUE(registered);
        EXPECT_TRUE(service.set_projection_mode(owner, "projection_ready"));
        EXPECT_TRUE(service.start_projection(owner));
        return registered ? registered.value() : aa::core::ConsumerId{};
    }
    void step(unsigned turns = 1) {
        for (unsigned i = 0; i < turns; ++i) {
            EXPECT_FALSE(supervisor.step(aa::core::Milliseconds{0}).has_value());
        }
    }
    void approve_wired(std::uint64_t id = 7) {
        approved.push_back({{id}, kWiredIdentity, aa::trust::IdentityStrength::serial_backed});
    }
    void attach(std::uint64_t id = 7) { attached = {{{id}, "1-2"}}; }
    void approve_wireless(std::uint64_t id, const aa::trust::TransportIdentity& identity) {
        approved.push_back({{id}, identity, aa::trust::IdentityStrength::serial_backed});
    }
    bool step_until(const std::function<bool()>& done, std::chrono::milliseconds limit = 2s) {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= until) return false;
            step();
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }
};

TEST(ManagedSupervisor, ModeAndTransportChangesStartAndReapTheWiredWorker) {
    Harness h;
    h.approve_wired();
    h.attach();
    h.step();
    EXPECT_EQ(h.worker_starts, 0U);
    EXPECT_EQ(h.releases, 1U);  // pairing_only holds no listeners

    const auto consumer = h.start();
    h.step();
    EXPECT_EQ(h.worker_starts, 1U);
    EXPECT_EQ(h.policy.active_transport, ipc::ProjectionTransport::wired);

    h.idr_requests = 0;
    ASSERT_TRUE(h.service.request_idr_recovery(h.owner));
    h.step();
    EXPECT_EQ(h.idr_requests, 1U);

    // Disabling the active transport stops projection and reaps the worker.
    ASSERT_TRUE(h.service.set_transport_enabled(h.owner, "wired", false));
    EXPECT_EQ(h.service.state().projection, ipc::ProjectionState::stopped);
    h.step();
    EXPECT_FALSE(h.worker_running);
    EXPECT_EQ(h.worker_stops, 1U);
    EXPECT_FALSE(h.policy.active_transport);

    // Back on, a new StartProjection projects again; pairing_only reaps it.
    ASSERT_TRUE(h.service.set_transport_enabled(h.owner, "wired", true));
    ASSERT_TRUE(h.service.start_projection(h.owner));
    h.step();
    EXPECT_EQ(h.worker_starts, 2U);
    ASSERT_TRUE(h.service.set_projection_mode(h.owner, "pairing_only"));
    h.step();
    EXPECT_FALSE(h.worker_running);
    EXPECT_EQ(h.worker_stops, 2U);
    EXPECT_EQ(h.service.state().consumer, consumer);
}

TEST(ManagedSupervisor, StopProjectionAndOwnerLossReapTheWiredWorker) {
    Harness h;
    h.approve_wired();
    h.attach();
    h.start();
    h.step();
    ASSERT_TRUE(h.worker_running);
    ASSERT_TRUE(h.service.stop_projection(h.owner));
    h.step();
    EXPECT_FALSE(h.worker_running);
    EXPECT_EQ(h.worker_stops, 1U);

    ASSERT_TRUE(h.service.start_projection(h.owner));
    h.step();
    ASSERT_TRUE(h.worker_running);
    ASSERT_TRUE(h.service.unregister_consumer(h.owner));
    h.step();
    EXPECT_FALSE(h.worker_running);
    EXPECT_EQ(h.worker_stops, 2U);
}

TEST(ManagedSupervisor, WirelessFailureKeepsRegistrationModeAndTransports) {
    Harness h;
    h.approve_wireless(9, kWirelessA);
    h.wireless = [](const wr::WirelessKeep&) { return 1; };  // e.g. an audio sink failure
    const auto consumer = h.start();
    ASSERT_TRUE(h.service.set_transport_enabled(h.owner, "wired", false));
    h.step();
    EXPECT_EQ(h.wireless_runs, 1U);
    // Contained to the transport: the supervisor goes on with the same state.
    EXPECT_EQ(h.service.state().consumer, consumer);
    EXPECT_EQ(h.service.state().projection, ipc::ProjectionState::projecting);
    EXPECT_TRUE(h.policy.ready);
    EXPECT_FALSE(h.policy.wired);
    EXPECT_FALSE(h.policy.active_transport);
    // Backed off, then retried.
    h.step();
    EXPECT_EQ(h.wireless_runs, 1U);
    EXPECT_TRUE(h.step_until([&] { return h.wireless_runs >= 2; }));

    // A process-level stop (signal, stdout log) still ends the supervisor.
    h.process_running = false;
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(h.supervisor.step(aa::core::Milliseconds{0}), std::optional<int>{1});
}

TEST(ManagedSupervisor, FailingWiredPhoneFallsBackToWirelessThenRetriesAfterReplug) {
    Harness h;
    h.approve_wired();
    h.approve_wireless(9, kWirelessA);
    h.attach();
    // Wireless waits with no session until `keep` ends it or the cap passes.
    unsigned preempted = 0;
    unsigned waited_out = 0;
    h.wireless = [&](const wr::WirelessKeep& keep) {
        const auto until = std::chrono::steady_clock::now() + 100ms;
        while (keep(h.service.state(), std::nullopt)) {
            if (std::chrono::steady_clock::now() >= until) {
                ++waited_out;
                return 0;
            }
            std::this_thread::sleep_for(1ms);
        }
        ++preempted;
        return 0;
    };
    h.start();
    // The worker exits at once each time (locked phone, admission refused).
    // Wireless waits during each backoff and gives way when wired may retry.
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        ASSERT_TRUE(h.step_until([&] { return h.worker_starts == attempt; })) << attempt;
        h.worker_running = false;
        h.step();
    }
    EXPECT_EQ(h.supervisor.arbiter().wired_failures({7}), 3U);
    EXPECT_EQ(waited_out, 0U);
    EXPECT_GE(preempted, 2U);
    // Parked: wireless keeps waiting while the failing phone stays plugged in.
    h.step();
    EXPECT_EQ(waited_out, 1U);
    EXPECT_EQ(h.worker_starts, 3U);
    EXPECT_EQ(h.service.state().projection, ipc::ProjectionState::projecting);

    // Unplugged long enough, then plugged in again: wired gets a fresh try.
    h.attached.clear();
    h.step();
    h.attach();
    ASSERT_TRUE(h.step_until([&] { return h.worker_starts == 4; }));
}

TEST(ManagedSupervisor, PhonePluggedInWhileWirelessWaitsTakesOver) {
    Harness h;
    h.approve_wired();
    h.approve_wireless(9, kWirelessA);
    unsigned turns = 0;
    h.wireless = [&](const wr::WirelessKeep& keep) {
        // Waits with no session; the phone is plugged in on the fifth turn.
        while (keep(h.service.state(), std::nullopt)) {
            if (++turns == 5) h.attach();
            if (turns > 1000) return 9;
        }
        return 0;
    };
    h.start();
    h.step();
    EXPECT_EQ(h.wireless_runs, 1U);
    EXPECT_GE(turns, 5U);
    EXPECT_LT(turns, 1000U);
    h.step();
    EXPECT_EQ(h.worker_starts, 1U);
    EXPECT_EQ(h.policy.active_transport, ipc::ProjectionTransport::wired);

    // A live wireless session is never preempted.
    Harness live;
    live.approve_wired();
    live.approve_wireless(9, kWirelessA);
    live.attach();
    live.wireless_idle = false;
    live.policy.wired = true;
    live.start();
    ASSERT_TRUE(live.service.set_transport_enabled(live.owner, "wired", false));
    unsigned live_turns = 0;
    live.wireless = [&](const wr::WirelessKeep& keep) {
        live.policy.wired = true;  // re-enabled mid-session
        while (keep(live.service.state(), kWirelessA) && ++live_turns < 20) {}
        return 0;
    };
    live.step();
    EXPECT_EQ(live_turns, 20U);
    EXPECT_EQ(live.worker_starts, 0U);
}

TEST(ManagedSupervisor, AnyApprovedWirelessPhoneProjects) {
    Harness h;
    h.approve_wireless(9, kWirelessA);
    h.approve_wireless(10, kWirelessB);
    std::vector<bool> kept;
    h.wireless = [&](const wr::WirelessKeep& keep) {
        // The second phone is the one the helper observed and admitted.
        kept.push_back(keep(h.service.state(), std::nullopt));
        kept.push_back(keep(h.service.state(), kWirelessB));
        h.approved.erase(h.approved.begin());  // forgetting the first phone
        kept.push_back(keep(h.service.state(), kWirelessB));
        h.approved.erase(h.approved.begin());  // forgetting the admitted phone
        kept.push_back(keep(h.service.state(), kWirelessB));
        return 0;
    };
    h.start();
    h.step();
    EXPECT_EQ(kept, (std::vector<bool>{true, true, true, false}));
}

TEST(ManagedSupervisor, SpawnFailureBacksOffAndReportsRarely) {
    Harness h;
    h.approve_wired();
    h.attach();
    h.spawn_fails = true;
    h.start();
    ASSERT_TRUE(h.service.set_transport_enabled(h.owner, "wireless", false));
    const auto until = std::chrono::steady_clock::now() + 200ms;
    unsigned turns = 0;
    while (std::chrono::steady_clock::now() < until) {
        h.step();
        ++turns;
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_GT(turns, 50U);
    // 10 ms doubling to a 40 ms cap within 200 ms: a handful, not one per turn.
    EXPECT_GE(h.reports.size(), 2U);
    EXPECT_LE(h.reports.size(), 9U);
}

TEST(ManagedSupervisor, BackoffArbiterParksAndResetsOnANewRequest) {
    wr::TransportArbiter arbiter{fast_policy()};
    const wr::WiredCandidate phone{{7}, "1-2"};
    auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(arbiter.wired_allowed(phone, now, true));
        arbiter.wired_started(phone, now);
        arbiter.wired_exited(now + 1ms);
        now += 1s;
    }
    EXPECT_FALSE(arbiter.wired_allowed(phone, now, true));
    EXPECT_TRUE(arbiter.wired_allowed(phone, now, false)) << "no wireless to fall back to";
    arbiter.reset();
    EXPECT_TRUE(arbiter.wired_allowed(phone, now, true));

    // A healthy run clears the failures.
    arbiter.wired_started(phone, now);
    arbiter.wired_exited(now + 1ms);
    arbiter.wired_started(phone, now + 1s);
    arbiter.wired_exited(now + 2s);
    EXPECT_EQ(arbiter.wired_failures(phone.phone), 0U);
}

} // namespace
