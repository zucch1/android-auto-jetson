// SPDX-License-Identifier: GPL-3.0-or-later
// The packaged supervisor loop (#221, #240 review P3-7) driven through its
// ports: a real ControlService, fake wired worker, fake wireless run and a
// scripted USB scan.
#include "../../tools/wired-receiver/VideoOutlet.hpp"
#include "../../tools/wireless-receiver/ManagedVideo.hpp"
#include "../../tools/wireless-receiver/Supervisor.hpp"
#include "../dbus/fakes.hpp"

#include <aa/ipc/VideoChannel.hpp>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <poll.h>
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
    unsigned video_accepts{};
    unsigned video_releases{};
    bool process_running{true};
    bool wireless_idle{true};
    std::vector<std::string> reports{};
    wr::Supervisor::Clock::time_point fake_now{100s};
    bool use_fake_clock{false};
    // The wireless run: returns its status after `keep` says stop, or at once.
    std::function<int(const wr::WirelessKeep&)> wireless = [](const wr::WirelessKeep&) { return 0; };

    wr::Supervisor supervisor;

    explicit Harness(wr::ArbitrationPolicy arbitration = fast_policy(), bool fake_clock = false)
        : use_fake_clock(fake_clock),
          supervisor(wr::SupervisorPorts{
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
              [this] { return use_fake_clock ? fake_now : wr::Supervisor::Clock::now(); },
              [this] { ++video_accepts; },
              [this] { ++video_releases; },
          }, arbitration) {
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
    auto policy = fast_policy();
    policy.max_backoff = 250ms;
    Harness h{policy};
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
    wr::TransportArbiter arbiter{wr::ArbitrationPolicy{}};
    const wr::WiredCandidate phone{{7}, "1-2"};
    auto now = wr::TransportArbiter::Clock::time_point{100s};
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(arbiter.wired_allowed(phone, now, true));
        arbiter.wired_started(phone, now);
        arbiter.wired_exited(now + 1s);
        now += 5s;
    }
    // Parked after 3 failures: yields to wireless before max_backoff (30 s),
    // but still allowed when wireless cannot run once normal backoff (4 s) elapsed.
    EXPECT_FALSE(arbiter.wired_allowed(phone, now, true));
    EXPECT_TRUE(arbiter.wired_allowed(phone, now, false)) << "no wireless to fall back to";
    // Bounded periodic re-probe opens at max_backoff (30 s) after the last failure.
    EXPECT_FALSE(arbiter.wired_allowed(phone, now + 25s, true));
    EXPECT_TRUE(arbiter.wired_allowed(phone, now + 26s, true));
    // A new StartProjection resets parked state immediately.
    arbiter.reset();
    EXPECT_TRUE(arbiter.wired_allowed(phone, now, true));

    // A healthy run clears the failures.
    arbiter.wired_started(phone, now);
    arbiter.wired_exited(now + 1s);
    arbiter.wired_started(phone, now + 2s);
    arbiter.wired_exited(now + 35s);
    EXPECT_EQ(arbiter.wired_failures(phone.phone), 0U);
}

TEST(ManagedSupervisor, ParkedWiredPhoneRecoversPeriodicallyWhenWirelessFailsOrIsUnreachable) {
    Harness h{wr::ArbitrationPolicy{}, /*fake_clock=*/true};
    h.approve_wired();
    h.approve_wireless(9, kWirelessA);
    h.attach();
    h.start();

    // Three short wired failures park the attached phone without unplugging it.
    // Backoffs between attempts 1->2 and 2->3 are 1 s and 2 s.
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        h.step();
        ASSERT_EQ(h.worker_starts, attempt);
        h.fake_now += 1s;
        h.worker_running = false;
        h.step();
        if (attempt == 1) h.fake_now += 1s;
        if (attempt == 2) h.fake_now += 2s;
    }
    ASSERT_EQ(h.supervisor.arbiter().wired_failures({7}), 3U);

    // Phase 1: approved wireless phone keeps failing (e.g. audio setup error).
    h.wireless = [](const wr::WirelessKeep&) { return 3; };
    h.step();
    EXPECT_EQ(h.wireless_runs, 1U);
    EXPECT_EQ(h.worker_starts, 3U);

    // During the 30 s parked window, wireless retries after backoff but wired stays parked.
    h.fake_now += 10s;
    h.step();
    EXPECT_EQ(h.wireless_runs, 2U);
    EXPECT_EQ(h.worker_starts, 3U);

    h.fake_now += 15s;  // 25 s since 3rd wired failure (< 30 s max_backoff)
    h.step();
    EXPECT_EQ(h.wireless_runs, 3U);
    EXPECT_EQ(h.worker_starts, 3U);

    // At 30 s, the parked wired phone gets a bounded periodic re-probe.
    h.fake_now += 5s;
    h.step();
    EXPECT_EQ(h.worker_starts, 4U);
    EXPECT_EQ(h.policy.active_transport, ipc::ProjectionTransport::wired);

    // Suppose the 4th probe still fails; next probe is pushed out by another 30 s.
    h.fake_now += 1s;
    h.worker_running = false;
    h.step();
    ASSERT_EQ(h.supervisor.arbiter().wired_failures({7}), 4U);

    // Phase 2: approved wireless phone is unreachable / helper cannot admit (#244),
    // so wireless sits idle (`wireless_idle == true`).
    unsigned idle_checks = 0;
    h.wireless_idle = true;
    h.wireless = [&](const wr::WirelessKeep& keep) {
        while (keep(h.service.state(), std::nullopt)) {
            ++idle_checks;
            h.fake_now += 5s;
            if (idle_checks > 20) return 9;
        }
        return 0;
    };
    // Advance past wireless backoff (4 s after 3 failures) without reaching 30 s.
    h.fake_now += 5s;
    h.step();
    // Checks at 5s, 10s, 15s, 20s, 25s stay true; at 30s the parked probe ends the idle wait.
    EXPECT_EQ(idle_checks, 5U);
    EXPECT_EQ(h.worker_starts, 4U);

    // Next step starts the wired phone with NO replug; this time it stays healthy.
    h.step();
    EXPECT_EQ(h.worker_starts, 5U);
    EXPECT_TRUE(h.worker_running);
    EXPECT_EQ(h.policy.active_transport, ipc::ProjectionTransport::wired);

    // Running past healthy_run clears the failure count on eventual exit.
    h.fake_now += 35s;
    h.step();
    EXPECT_TRUE(h.worker_running);
    h.worker_running = false;
    h.step();
    EXPECT_EQ(h.supervisor.arbiter().wired_failures({7}), 0U);
}

TEST(ManagedSupervisor, LiveWirelessSessionNeverPreemptedByParkedWiredProbe) {
    Harness h{wr::ArbitrationPolicy{}, /*fake_clock=*/true};
    h.approve_wired();
    h.approve_wireless(9, kWirelessA);
    h.attach();
    h.start();

    // Park the attached wired phone after 3 short failures.
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        h.step();
        ASSERT_EQ(h.worker_starts, attempt);
        h.fake_now += 1s;
        h.worker_running = false;
        h.step();
        if (attempt == 1) h.fake_now += 1s;
        if (attempt == 2) h.fake_now += 2s;
    }
    ASSERT_EQ(h.supervisor.arbiter().wired_failures({7}), 3U);

    // Wireless enters bootstrap and then a live session (`wireless_idle == false`).
    // Advancing well past the 30 s parked probe interval must never preempt it.
    std::vector<bool> kept;
    h.wireless_idle = false;
    h.wireless = [&](const wr::WirelessKeep& keep) {
        // Bootstrap in progress past 30 s: never preempted.
        h.fake_now += 35s;
        kept.push_back(keep(h.service.state(), std::nullopt));
        // Live admitted session past 60 s and 120 s: never preempted.
        h.fake_now += 30s;
        kept.push_back(keep(h.service.state(), kWirelessA));
        h.fake_now += 60s;
        kept.push_back(keep(h.service.state(), kWirelessA));
        return 0;
    };
    h.step();
    EXPECT_EQ(h.wireless_runs, 1U);
    EXPECT_EQ(kept, (std::vector<bool>{true, true, true}));
    EXPECT_EQ(h.worker_starts, 3U);

    // Once the live wireless session ends on its own, the parked wired phone may probe.
    h.step();
    EXPECT_EQ(h.worker_starts, 4U);
    EXPECT_EQ(h.policy.active_transport, ipc::ProjectionTransport::wired);
}

TEST(ManagedSupervisor, QuickWiredExitWithWirelessDisabledRetriesAfterBackoffAndKeepsVideo) {
    Harness h{wr::ArbitrationPolicy{}, /*fake_clock=*/true};
    h.approve_wired();
    h.attach();
    h.start();
    ASSERT_TRUE(h.service.set_transport_enabled(h.owner, "wireless", false));

    // First StartProjection starts the wired worker and accepts video promptly.
    h.step();
    EXPECT_EQ(h.worker_starts, 1U);
    EXPECT_EQ(h.video_accepts, 1U);
    EXPECT_EQ(h.video_releases, 0U);
    EXPECT_EQ(h.policy.active_transport, ipc::ProjectionTransport::wired);

    // Worker exits quickly (~1.4 s, e.g. USB reset/re-enumeration failure).
    // Projection stays requested and the accepted video socket is NOT closed.
    h.fake_now += 1400ms;
    h.worker_running = false;
    h.step();
    EXPECT_EQ(h.service.state().projection, ipc::ProjectionState::projecting);
    EXPECT_FALSE(h.policy.active_transport);
    EXPECT_EQ(h.video_releases, 0U);
    EXPECT_EQ(h.supervisor.arbiter().wired_failures({7}), 1U);

    // During the 1 s backoff, the supervisor keeps accepting video and does not
    // spawn early.
    h.fake_now += 500ms;
    h.step();
    EXPECT_EQ(h.worker_starts, 1U);
    EXPECT_GE(h.video_accepts, 2U);

    // At 1 s backoff, the second wired attempt launches automatically.
    h.fake_now += 500ms;
    h.step();
    EXPECT_EQ(h.worker_starts, 2U);
    EXPECT_EQ(h.policy.active_transport, ipc::ProjectionTransport::wired);

    // With wireless disabled, wired keeps retrying with exponential backoff
    // (2 s, then 4 s) and is never parked after 3 failures.
    h.fake_now += 1s;
    h.worker_running = false;
    h.step();
    h.fake_now += 2s;
    h.step();
    EXPECT_EQ(h.worker_starts, 3U);

    h.fake_now += 1s;
    h.worker_running = false;
    h.step();
    ASSERT_EQ(h.supervisor.arbiter().wired_failures({7}), 3U);
    h.fake_now += 3s;
    h.step();
    EXPECT_EQ(h.worker_starts, 3U);
    h.fake_now += 1s;
    h.step();
    EXPECT_EQ(h.worker_starts, 4U);
    EXPECT_EQ(h.video_releases, 0U);

    // A healthy run (>= 30 s) clears failures and still keeps the supervisor's
    // video connection open while projection remains requested.
    h.fake_now += 35s;
    h.worker_running = false;
    h.step();
    EXPECT_EQ(h.supervisor.arbiter().wired_failures({7}), 0U);
    EXPECT_EQ(h.video_releases, 0U);

    // StopProjection releases video and stops retrying.
    ASSERT_TRUE(h.service.stop_projection(h.owner));
    h.step();
    EXPECT_EQ(h.video_releases, 1U);
    EXPECT_EQ(h.worker_starts, 4U);
}

namespace {

struct VideoScratch final {
    VideoScratch() {
        char name[] = "/tmp/aa-sup-video-XXXXXX";
        if (const auto* directory = ::mkdtemp(name)) path = directory;
    }
    ~VideoScratch() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    std::filesystem::path path;
};

std::vector<std::byte> idr_au(std::uint8_t tag) {
    return {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
            std::byte{0x65}, std::byte{tag}};
}

std::vector<std::byte> delta_au(std::uint8_t tag) {
    return {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
            std::byte{0x41}, std::byte{tag}};
}

aa::core::Result<ipc::ReceiveStatus> recv_complete(ipc::VideoConsumer& consumer) {
    for (int i = 0; i < 16; ++i) {
        auto step = consumer.receive(aa::core::Milliseconds{2000});
        if (!step) return step;
        if (step.value() == ipc::ReceiveStatus::complete) return step;
    }
    return aa::Error{aa::ErrorCode::timeout};
}

// Connects a real VideoConsumer against the supervisor's real VideoServer by
// stepping the supervisor (no timed polling sleeps).
aa::core::Result<ipc::VideoConsumer> connect_via_supervisor(
    const ipc::VideoSocketPath& video_path, ipc::VideoServer& server,
    wr::ManagedVideoBroker& broker, const std::function<void()>& step_fn) {
    const auto before = broker.generation();
    aa::core::Result<ipc::VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::thread connector([&] {
        consumer = ipc::connect_video_consumer(
            video_path, {{}, ipc::current_uid(), aa::core::Milliseconds{2000}});
    });
    while (broker.generation() == before) {
        if (!broker.has_pending_handshake(server)) {
            pollfd pfd{server.native_handle(), POLLIN, 0};
            (void)::poll(&pfd, 1, 500);
        }
        step_fn();
    }
    connector.join();
    return consumer;
}

struct RealVideoHarness final {
    VideoScratch scratch{};
    ipc::VideoSocketPath video_path{
        (scratch.path / "android-auto-receiver" / "video.sock").string()};
    std::optional<ipc::VideoServer> server{};
    wr::ManagedVideoBroker broker{};
    ipc::test::FakePeerCredentials credentials{};
    ipc::test::RecordingEventSink events{};
    ipc::test::SequencePairingIds ids{};
    ipc::test::RecordingPhoneDirectory phones{};
    ipc::ProjectionPolicy policy{};
    ipc::ControlService service{{kUid}, {credentials, events, ids, phones, nullptr, {},
                                         [this] { return worker_running; }, &policy}};
    const ipc::BusName owner{":1.41"};
    const ipc::BusName name{"org.custom.Consumer"};

    std::vector<aa::trust::ApprovedPhone> approved{};
    std::vector<wr::WiredCandidate> attached{};
    bool worker_running{};
    unsigned worker_starts{};
    unsigned wireless_runs{};
    bool wireless_idle{true};
    wr::Supervisor::Clock::time_point fake_now{100s};
    std::optional<aa::wired_receiver::VideoOutlet> wired_outlet{};
    std::uint64_t wired_video_gen{0};
    std::function<int(const wr::WirelessKeep&)> wireless_fn{};
    wr::Supervisor supervisor;

    RealVideoHarness()
        : supervisor(wr::SupervisorPorts{
              [](aa::core::Milliseconds) -> aa::core::Result<void> { return {}; },
              [this]() -> ipc::ControlService& { return service; },
              policy,
              [this]() -> const std::vector<aa::trust::ApprovedPhone>& { return approved; },
              [this](const std::optional<wr::WiredCandidate>&) { return attached; },
              [this](const wr::WiredCandidate&) {
                  ++worker_starts;
                  worker_running = true;
                  wired_outlet.emplace([](const std::string&) {});
                  wired_video_gen = 0;
                  sync_wired();
                  return true;
              },
              [this] { return worker_running; },
              [this] {
                  worker_running = false;
                  wired_outlet.reset();
              },
              [] {},
              [this](const wr::WirelessKeep& keep) {
                  ++wireless_runs;
                  return wireless_fn ? wireless_fn(keep) : 0;
              },
              [this] { return wireless_idle; },
              [this] {
                  broker.release_connection(server ? &*server : nullptr);
                  server.reset();
              },
              [] { return true; },
              [](const char*, ErrorCode) {},
              [this] { return fake_now; },
              [this] {
                  if (!server) return;
                  (void)broker.poll(*server);
                  sync_wired();
              },
              [this] {
                  broker.release_connection(server ? &*server : nullptr);
                  wired_video_gen = broker.generation();
              },
          }, wr::ArbitrationPolicy{}) {
        credentials.add_connection(owner, kUid);
        credentials.set_owner(name, owner);
        policy.check_ready = [this]() -> aa::core::Result<void> {
            if (!server) {
                auto opened = ipc::VideoServer::listen(video_path, {{}, ipc::current_uid()});
                if (!opened) return opened.error();
                server.emplace(std::move(opened.value()));
            }
            return {};
        };
    }

    void sync_wired() {
        if (!worker_running || !wired_outlet) return;
        const auto gen = broker.generation();
        if (gen == wired_video_gen) return;
        if (broker.has_connection()) {
            auto borrowed = broker.borrow();
            if (borrowed) {
                wired_outlet->install(std::move(borrowed.value()));
                wired_video_gen = gen;
            }
        } else {
            wired_outlet->clear_producer();
            wired_video_gen = gen;
        }
    }

    void start() {
        ASSERT_TRUE(service.register_consumer(owner, name));
        ASSERT_TRUE(service.set_projection_mode(owner, "projection_ready"));
        ASSERT_TRUE(service.start_projection(owner));
    }

    void step() { EXPECT_FALSE(supervisor.step(aa::core::Milliseconds{0}).has_value()); }

    void exit_wired() {
        worker_running = false;
        wired_outlet.reset();  // closes borrowed VideoProducer without SHUT_WR
    }
};

} // namespace

TEST(ManagedSupervisor, LateConnectSurvivesFailedFirstWiredWorkerAndDeliversSecondWorkerIdr) {
    RealVideoHarness h;
    h.approved.push_back({{7}, kWiredIdentity, aa::trust::IdentityStrength::serial_backed});
    h.attached = {{{7}, "1-2"}};
    h.start();
    ASSERT_TRUE(h.service.set_transport_enabled(h.owner, "wireless", false));

    // Step 1 spawns worker 1 BEFORE the display connects to video.sock (P1-1).
    h.step();
    ASSERT_EQ(h.worker_starts, 1U);
    ASSERT_TRUE(h.worker_running);
    ASSERT_FALSE(h.broker.has_connection());

    // Display connects while worker 1 is running; supervisor owns the socket.
    auto consumer = connect_via_supervisor(h.video_path, *h.server, h.broker, [&] { h.step(); });
    ASSERT_TRUE(consumer);
    ASSERT_TRUE(h.broker.has_connection());

    // Worker 1 fails USB startup after 1.4 s without sending frames.
    h.fake_now += 1400ms;
    h.exit_wired();
    h.step();
    ASSERT_EQ(h.supervisor.arbiter().wired_failures({7}), 1U);
    EXPECT_TRUE(h.broker.has_connection());

    // After 1 s backoff, worker 2 spawns, borrows the original connection, and
    // sends an IDR that arrives on the original VideoConsumer.
    h.fake_now += 1s;
    h.step();
    ASSERT_EQ(h.worker_starts, 2U);
    aa::protocol::MessageHeader hdr{};
    hdr.timestamp = aa::core::Nanoseconds{1'000'000};
    ASSERT_TRUE(h.wired_outlet->on_message({hdr, idr_au(0x51)}));

    ASSERT_TRUE(recv_complete(consumer.value()));
    EXPECT_EQ(consumer.value().access_unit().frame, 1U);
    EXPECT_EQ(consumer.value().access_unit().payload.back(), 0x51U);
    EXPECT_EQ(consumer.value().rejected(), 0U);
}

TEST(ManagedSupervisor, ShortStreamingWiredHandsOffToRetryAndWirelessWithMonotonicIds) {
    RealVideoHarness h;
    h.approved.push_back({{7}, kWiredIdentity, aa::trust::IdentityStrength::serial_backed});
    h.approved.push_back({{9}, kWirelessA, aa::trust::IdentityStrength::serial_backed});
    h.attached = {{{7}, "1-2"}};
    h.start();

    // Pre-connect consumer, then start worker 1.
    auto consumer = connect_via_supervisor(h.video_path, *h.server, h.broker, [&] {
        (void)h.broker.poll(*h.server);
    });
    ASSERT_TRUE(consumer);

    // Worker 1 sends a real IDR + delta, then exits within 30 s.
    h.step();
    ASSERT_EQ(h.worker_starts, 1U);
    aa::protocol::MessageHeader hdr{};
    hdr.timestamp = aa::core::Nanoseconds{1'000'000};
    ASSERT_TRUE(h.wired_outlet->on_message({hdr, idr_au(0x11)}));
    hdr.timestamp = aa::core::Nanoseconds{34'000'000};
    ASSERT_TRUE(h.wired_outlet->on_message({hdr, delta_au(0x12)}));
    h.fake_now += 2s;
    h.exit_wired();
    h.step();

    ASSERT_TRUE(recv_complete(consumer.value()));
    const auto f1 = consumer.value().access_unit().frame;
    const auto s1 = consumer.value().access_unit().sequence;
    EXPECT_EQ(consumer.value().access_unit().payload.back(), 0x11U);
    ASSERT_TRUE(recv_complete(consumer.value()));
    const auto f2 = consumer.value().access_unit().frame;
    const auto s2 = consumer.value().access_unit().sequence;
    EXPECT_GT(f2, f1);
    EXPECT_GT(s2, s1);

    // During backoff, wireless runs and borrows the SAME connection via a fresh
    // VideoOutlet, sending an IDR with monotonic frame/sequence and 0 rejects.
    h.wireless_fn = [&](const wr::WirelessKeep&) {
        aa::wired_receiver::VideoOutlet wireless_outlet{[](const std::string&) {}};
        auto borrowed = h.broker.borrow();
        EXPECT_TRUE(borrowed);
        if (borrowed) {
            wireless_outlet.install(std::move(borrowed.value()));
            aa::protocol::MessageHeader whdr{};
            whdr.timestamp = aa::core::Nanoseconds{200'000'000};
            EXPECT_TRUE(wireless_outlet.on_message({whdr, idr_au(0x22)}));
        }
        wireless_outlet.close();  // borrowed close: must NOT send SHUT_WR
        return 0;
    };
    h.step();
    ASSERT_EQ(h.wireless_runs, 1U);

    ASSERT_TRUE(recv_complete(consumer.value()));
    const auto f3 = consumer.value().access_unit().frame;
    const auto s3 = consumer.value().access_unit().sequence;
    EXPECT_GT(f3, f2);
    EXPECT_GT(s3, s2);
    EXPECT_EQ(consumer.value().access_unit().payload.back(), 0x22U);
    EXPECT_EQ(consumer.value().rejected(), 0U);

    // Backoff expires: wired worker 2 borrows the SAME connection and sends IDR.
    h.fake_now += 1s;
    h.step();
    ASSERT_EQ(h.worker_starts, 2U);
    hdr.timestamp = aa::core::Nanoseconds{400'000'000};
    ASSERT_TRUE(h.wired_outlet->on_message({hdr, idr_au(0x33)}));

    ASSERT_TRUE(recv_complete(consumer.value()));
    EXPECT_GT(consumer.value().access_unit().frame, f3);
    EXPECT_GT(consumer.value().access_unit().sequence, s3);
    EXPECT_EQ(consumer.value().access_unit().payload.back(), 0x33U);
    EXPECT_EQ(consumer.value().rejected(), 0U);

    // StopProjection sends SHUT_WR -> consumer sees clean EOF.
    ASSERT_TRUE(h.service.stop_projection(h.owner));
    h.step();
    const auto eof = consumer.value().receive(aa::core::Milliseconds{500});
    ASSERT_FALSE(eof);
    EXPECT_EQ(eof.error().code(), ErrorCode::transport_closed);
}

TEST(ManagedSupervisor, ConsumerDisconnectAndReconnectReplacedUnderOneRegistration) {
    RealVideoHarness h;
    h.approved.push_back({{7}, kWiredIdentity, aa::trust::IdentityStrength::serial_backed});
    h.attached = {{{7}, "1-2"}};
    h.start();
    ASSERT_TRUE(h.service.set_transport_enabled(h.owner, "wireless", false));

    h.step();
    ASSERT_EQ(h.worker_starts, 1U);

    auto consumer1 = connect_via_supervisor(h.video_path, *h.server, h.broker, [&] { h.step(); });
    ASSERT_TRUE(consumer1);
    aa::protocol::MessageHeader hdr{};
    hdr.timestamp = aa::core::Nanoseconds{1'000'000};
    ASSERT_TRUE(h.wired_outlet->on_message({hdr, idr_au(0x61)}));
    ASSERT_TRUE(recv_complete(consumer1.value()));
    EXPECT_EQ(consumer1.value().access_unit().payload.back(), 0x61U);

    // Consumer 1 disconnects while the D-Bus registration stays live.
    consumer1.value().close();
    h.step();
    EXPECT_FALSE(h.broker.has_connection());

    // Consumer 2 reconnects under the same registration; supervisor detects the
    // new peer and forwards it to the running worker (and future retries).
    auto consumer2 = connect_via_supervisor(h.video_path, *h.server, h.broker, [&] { h.step(); });
    ASSERT_TRUE(consumer2);
    ASSERT_TRUE(h.broker.has_connection());

    hdr.timestamp = aa::core::Nanoseconds{100'000'000};
    ASSERT_TRUE(h.wired_outlet->on_message({hdr, idr_au(0x62)}));
    ASSERT_TRUE(recv_complete(consumer2.value()));
    EXPECT_EQ(consumer2.value().access_unit().payload.back(), 0x62U);
    EXPECT_EQ(consumer2.value().rejected(), 0U);
}

TEST(ManagedSupervisor, IdleWirelessPreemptionAndAudioFailurePreserveConsumerForWired) {
    RealVideoHarness h;
    h.approved.push_back({{7}, kWiredIdentity, aa::trust::IdentityStrength::serial_backed});
    h.approved.push_back({{9}, kWirelessA, aa::trust::IdentityStrength::serial_backed});
    h.start();

    // Consumer connects while wireless is about to wait (no wired phone yet).
    auto consumer = connect_via_supervisor(h.video_path, *h.server, h.broker, [&] {
        (void)h.broker.poll(*h.server);
    });
    ASSERT_TRUE(consumer);

    // First wireless run simulates audio-router startup failure (exit 3) before
    // installing a producer: consumer connection must stay alive.
    h.wireless_fn = [&](const wr::WirelessKeep&) { return 3; };
    h.step();
    ASSERT_EQ(h.wireless_runs, 1U);
    EXPECT_TRUE(h.broker.has_connection());

    // Second wireless run borrows the connection while idle, then a wired phone
    // is plugged in and preempts idle wireless.
    h.fake_now += 1s;
    h.wireless_idle = true;
    h.wireless_fn = [&](const wr::WirelessKeep& keep) {
        aa::wired_receiver::VideoOutlet wireless_outlet{[](const std::string&) {}};
        auto borrowed = h.broker.borrow();
        EXPECT_TRUE(borrowed);
        if (borrowed) wireless_outlet.install(std::move(borrowed.value()));
        EXPECT_TRUE(keep(h.service.state(), std::nullopt));
        h.attached = {{{7}, "1-2"}};  // wired phone plugged in!
        h.fake_now += 250ms;          // advance past ArbitrationPolicy::scan_interval
        EXPECT_FALSE(keep(h.service.state(), std::nullopt));
        wireless_outlet.close();  // borrowed close: must NOT send SHUT_WR
        return 0;
    };
    h.step();
    ASSERT_EQ(h.wireless_runs, 2U);
    EXPECT_TRUE(h.broker.has_connection());

    // Next step starts the wired worker, which borrows the original connection
    // and delivers the first IDR to the original VideoConsumer.
    h.step();
    ASSERT_EQ(h.worker_starts, 1U);
    aa::protocol::MessageHeader hdr{};
    hdr.timestamp = aa::core::Nanoseconds{1'000'000};
    ASSERT_TRUE(h.wired_outlet->on_message({hdr, idr_au(0x77)}));

    ASSERT_TRUE(recv_complete(consumer.value()));
    EXPECT_EQ(consumer.value().access_unit().payload.back(), 0x77U);
    EXPECT_EQ(consumer.value().rejected(), 0U);
}

} // namespace
