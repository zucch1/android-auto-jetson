// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The packaged supervisor's projection loop (#221): one registered consumer,
// one pair of media listeners, wireless in-process and a managed wired worker.
// The loop is written against SupervisorPorts so tests can drive it without a
// phone, a helper or a real worker.

#include "WiredWorker.hpp"

#include <aa/core/Time.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/trust/Store.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace aa::wireless_receiver {

// Transport arbitration while projection is requested (#240 review P2-4).
//
// - Wired goes first when an approved phone is attached and its retry time has
//   come. Wireless runs otherwise.
// - A transport failure never ends the requested projection. A wired worker
//   that exits before `healthy_run` counts as a failed attempt for its phone and
//   backs off (`first_backoff`, doubling up to `max_backoff`). After
//   `wired_attempts` such failures in a row the phone is parked: it no longer
//   holds wireless back. A parked phone is tried again once it has been absent
//   from USB for `absent_reset` (it was unplugged and plugged in again), on a
//   new StartProjection, or on a bounded periodic re-probe every `max_backoff`
//   whenever no wireless session or bootstrap is live (a live wireless session
//   is never preempted). When wireless cannot run at all, wired keeps retrying
//   under its normal exponential backoff up to `max_backoff`.
// - A wireless run that fails backs off the same way, and wired keeps its turn.
// - While wireless waits for a phone with no session or bootstrap in progress,
//   the supervisor keeps scanning USB. An attached wired phone that is allowed
//   to run ends that wait and takes over.
struct ArbitrationPolicy final {
    std::chrono::milliseconds healthy_run{30'000};
    std::chrono::milliseconds first_backoff{1'000};
    std::chrono::milliseconds max_backoff{30'000};
    unsigned wired_attempts{3};
    std::chrono::milliseconds absent_reset{3'000};
    std::chrono::milliseconds scan_interval{250};
};

class TransportArbiter final {
public:
    using Clock = std::chrono::steady_clock;

    explicit TransportArbiter(ArbitrationPolicy policy = {}) : policy_(policy) {}

    // A new projection request: every transport gets a fresh chance.
    void reset() {
        wired_.clear();
        wireless_failures_ = 0;
        wireless_retry_ = {};
    }

    void observe_scan(const std::vector<WiredCandidate>& candidates, Clock::time_point now) {
        for (auto& [phone, state] : wired_) {
            const bool present = std::any_of(candidates.begin(), candidates.end(),
                [&](const auto& candidate) { return candidate.phone.value == phone; });
            if (present) {
                state.absent_since.reset();
            } else if (!state.absent_since) {
                state.absent_since = now;
            } else if (now - *state.absent_since >= policy_.absent_reset) {
                state = {};
                state.absent_since = now;
            }
        }
    }

    [[nodiscard]] bool wired_allowed(const WiredCandidate& candidate, Clock::time_point now,
                                     bool wireless_possible) const {
        const auto found = wired_.find(candidate.phone.value);
        if (found == wired_.end()) return true;
        const auto& state = found->second;
        if (state.failures >= policy_.wired_attempts && wireless_possible) {
            return now >= state.parked_retry;
        }
        return now >= state.retry;
    }

    void wired_started(const WiredCandidate& candidate, Clock::time_point now) {
        auto& state = wired_[candidate.phone.value];
        state.started = now;
        state.absent_since.reset();
        running_ = candidate.phone.value;
    }

    // The worker could not be spawned: a failed attempt without a run.
    void wired_spawn_failed(const WiredCandidate& candidate, Clock::time_point now) {
        fail(wired_[candidate.phone.value], now);
    }

    void wired_exited(Clock::time_point now) {
        if (!running_) return;
        auto& state = wired_[*running_];
        running_.reset();
        if (now - state.started >= policy_.healthy_run) {
            state.failures = 0;
            state.retry = now + policy_.first_backoff;
            state.parked_retry = {};
        } else {
            fail(state, now);
        }
    }

    [[nodiscard]] bool wireless_allowed(Clock::time_point now) const { return now >= wireless_retry_; }

    void wireless_finished(bool failed, Clock::time_point now) {
        if (!failed) {
            wireless_failures_ = 0;
            return;
        }
        wireless_retry_ = now + backoff(++wireless_failures_);
    }

    [[nodiscard]] unsigned wired_failures(core::PhoneId phone) const {
        const auto found = wired_.find(phone.value);
        return found == wired_.end() ? 0U : found->second.failures;
    }
    [[nodiscard]] const ArbitrationPolicy& policy() const noexcept { return policy_; }

private:
    struct Wired final {
        unsigned failures{};
        Clock::time_point retry{};
        Clock::time_point parked_retry{};
        Clock::time_point started{};
        std::optional<Clock::time_point> absent_since{};
    };

    [[nodiscard]] std::chrono::milliseconds backoff(unsigned failures) const {
        auto delay = policy_.first_backoff;
        for (unsigned i = 1; i < failures && delay < policy_.max_backoff; ++i) delay *= 2;
        return std::min(delay, policy_.max_backoff);
    }
    void fail(Wired& state, Clock::time_point now) {
        ++state.failures;
        state.retry = now + backoff(state.failures);
        state.parked_retry = now + policy_.max_backoff;
    }

    ArbitrationPolicy policy_;
    std::map<std::uint64_t, Wired> wired_{};
    std::optional<std::uint64_t> running_{};
    unsigned wireless_failures_{};
    Clock::time_point wireless_retry_{};
};

// Whether a wireless run keeps going: the current snapshot and the phone the
// run admitted (nullopt while it waits).
using WirelessKeep =
    std::function<bool(ipc::StateSnapshot, const std::optional<trust::TransportIdentity>&)>;

struct SupervisorPorts final {
    std::function<core::Result<void>(core::Milliseconds)> cycle;
    std::function<ipc::ControlService&()> service;
    ipc::ProjectionPolicy& policy;
    std::function<const std::vector<trust::ApprovedPhone>&()> approved;
    std::function<std::vector<WiredCandidate>(const std::optional<WiredCandidate>&)> scan_wired;
    std::function<bool(const WiredCandidate&)> start_wired;
    std::function<bool()> wired_running;
    std::function<void()> stop_wired;
    std::function<void()> wired_idr;
    // Blocking wireless projection. Returns its exit status.
    std::function<int(const WirelessKeep&)> run_wireless;
    // True while a wireless run waits with no session and no bootstrap.
    std::function<bool()> wireless_idle;
    std::function<void()> release_media;
    std::function<bool()> process_running;
    std::function<void(const char*, ErrorCode)> report;
    std::function<TransportArbiter::Clock::time_point()> now = TransportArbiter::Clock::now;
    std::function<void()> service_video{};
    std::function<void()> release_video{};
};

class Supervisor final {
public:
    using Clock = TransportArbiter::Clock;

    explicit Supervisor(SupervisorPorts ports, ArbitrationPolicy policy = {})
        : ports_(std::move(ports)), arbiter_(policy) {}

    // One dispatch turn. Returns an exit status when the supervisor must end.
    std::optional<int> step(core::Milliseconds wait) {
        if (auto cycled = ports_.cycle(wait); !cycled) {
            ports_.report("session bus dispatch", cycled.error().code());
            return 1;
        }
        auto& policy = ports_.policy;
        const auto state = ports_.service().state();
        const bool wants = state.consumer.value && policy.ready &&
                           state.projection == ipc::ProjectionState::projecting;
        if (!wants) {
            ports_.stop_wired();
            if (ports_.release_video) ports_.release_video();
            wired_.reset();
            request_.reset();
            policy.active_transport.reset();
            if (!policy.ready) ports_.release_media();
            return std::nullopt;
        }
        if (request_ != state.consumer) {
            if (request_.has_value() && ports_.release_video) {
                ports_.release_video();
            }
            request_ = state.consumer;
            arbiter_.reset();
            next_scan_ = {};
        }
        if (ports_.service_video) ports_.service_video();
        const auto& approved = ports_.approved();
        if (wired_) {
            if (!approved_phone(approved, wired_->phone)) ports_.stop_wired();
            if (ports_.wired_running()) {
                if (ports_.service().consume_idr_recovery_request()) ports_.wired_idr();
                return std::nullopt;
            }
            // The worker ended on its own (or for revocation). Projection stays
            // requested, the supervisor keeps owning the video connection (with
            // shared monotonic frame/sequence counters), and arbitration picks
            // the next transport.
            arbiter_.wired_exited(now());
            wired_.reset();
            policy.active_transport.reset();
            return std::nullopt;
        }

        if (start_wired_if_allowed(approved)) return std::nullopt;

        if (policy.wireless && arbiter_.wireless_allowed(now()) && has_wireless(approved)) {
            policy.active_transport = ipc::ProjectionTransport::wireless;
            const auto consumer = state.consumer;
            const auto result = ports_.run_wireless(
                [this, consumer](ipc::StateSnapshot current,
                                 const std::optional<trust::TransportIdentity>& admitted) {
                    return keep_wireless(consumer, current, admitted);
                });
            policy.active_transport.reset();
            arbiter_.wireless_finished(result != 0, now());
            const auto after = ports_.service().state();
            const bool wants_after = after.consumer.value && policy.ready &&
                                     after.projection == ipc::ProjectionState::projecting;
            if (!wants_after) {
                if (ports_.release_video) ports_.release_video();
                request_.reset();
            }
            if (!policy.ready) ports_.release_media();
            if (result != 0 && !ports_.process_running()) return result;
        }
        return std::nullopt;
    }

    [[nodiscard]] const TransportArbiter& arbiter() const noexcept { return arbiter_; }
    [[nodiscard]] const std::optional<WiredCandidate>& wired() const noexcept { return wired_; }

private:
    [[nodiscard]] Clock::time_point now() const {
        return ports_.now ? ports_.now() : Clock::now();
    }
    static bool approved_phone(const std::vector<trust::ApprovedPhone>& approved, core::PhoneId phone) {
        return std::any_of(approved.begin(), approved.end(),
                           [&](const auto& entry) { return entry.phone_id == phone; });
    }
    static bool has_wireless(const std::vector<trust::ApprovedPhone>& approved) {
        return std::any_of(approved.begin(), approved.end(), [](const auto& entry) {
            return std::holds_alternative<trust::WirelessIdentity>(entry.identity);
        });
    }
    static bool approves(const std::vector<trust::ApprovedPhone>& approved,
                         const trust::TransportIdentity& identity) {
        const auto key = trust::identity_key(identity);
        return std::any_of(approved.begin(), approved.end(), [&](const auto& entry) {
            return trust::identity_key(entry.identity) == key;
        });
    }
    [[nodiscard]] bool wireless_possible(const std::vector<trust::ApprovedPhone>& approved) const {
        return ports_.policy.wireless && has_wireless(approved);
    }

    // Scans at most every scan_interval. Returns the allowed candidate, if any.
    std::optional<WiredCandidate> allowed_candidate(const std::vector<trust::ApprovedPhone>& approved) {
        const auto current_time = now();
        if (!ports_.policy.wired || current_time < next_scan_) return std::nullopt;
        next_scan_ = current_time + arbiter_.policy().scan_interval;
        const auto candidates = ports_.scan_wired(last_wired_);
        arbiter_.observe_scan(candidates, current_time);
        const bool fallback = wireless_possible(approved);
        for (const auto& candidate : candidates) {
            if (arbiter_.wired_allowed(candidate, current_time, fallback)) return candidate;
        }
        return std::nullopt;
    }

    bool start_wired_if_allowed(const std::vector<trust::ApprovedPhone>& approved) {
        const auto candidate = allowed_candidate(approved);
        if (!candidate) return false;
        const auto current_time = now();
        if (!ports_.start_wired(*candidate)) {
            // Backoff limits the retry rate and with it this report.
            arbiter_.wired_spawn_failed(*candidate, current_time);
            ports_.report("wired projection worker", ErrorCode::transport_unavailable);
            return false;
        }
        wired_ = candidate;
        last_wired_ = candidate;
        arbiter_.wired_started(*candidate, current_time);
        ports_.policy.active_transport = ipc::ProjectionTransport::wired;
        return true;
    }

    bool keep_wireless(core::ConsumerId consumer, ipc::StateSnapshot current,
                       const std::optional<trust::TransportIdentity>& admitted) {
        const auto& policy = ports_.policy;
        const auto& approved = ports_.approved();
        if (!policy.ready || !policy.wireless || current.consumer != consumer ||
            current.projection != ipc::ProjectionState::projecting || !has_wireless(approved) ||
            (admitted && !approves(approved, *admitted))) {
            return false;
        }
        // A phone plugged in while wireless waits takes over.
        if (ports_.wireless_idle() && allowed_candidate(approved)) {
            next_scan_ = {};
            return false;
        }
        return true;
    }

    SupervisorPorts ports_;
    TransportArbiter arbiter_;
    std::optional<core::ConsumerId> request_{};
    std::optional<WiredCandidate> wired_{};
    std::optional<WiredCandidate> last_wired_{};
    Clock::time_point next_scan_{};
};

} // namespace aa::wireless_receiver
