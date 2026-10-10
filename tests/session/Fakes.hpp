// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test-only fakes + harness for the three narrow lifecycle seams (clock, trust,
// transport). The fakes record the orchestration tests assert on (bound token,
// open/close counts, approvals). The harness drives a Lifecycle to any state
// along a canonical legal path so property tests can start from every state.

#include <aa/session/Lifecycle.hpp>
#include <aa/trust/Trust.hpp>
#include <aa/transport/Transport.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aa::session::test {

inline const trust::PhoneIdentity kApprovedId{"phone-approved"};
inline const trust::PhoneIdentity kUnknownId{"phone-unknown"};
inline const trust::PhoneIdentity kRejectedId{"phone-rejected"};

class ManualClock final : public MonotonicClock {
public:
    [[nodiscard]] core::Nanoseconds now() const noexcept override { return now_; }
    void set(core::Nanoseconds t) noexcept { now_ = t; }
    void advance(core::Milliseconds delta) noexcept {
        now_ = core::Nanoseconds{now_.count + core::to_nanoseconds(delta).count};
    }

private:
    core::Nanoseconds now_{};
};

class MapTrustStore final : public trust::TrustStore {
public:
    [[nodiscard]] trust::Decision lookup(const trust::PhoneIdentity& identity) const override {
        const auto it = decisions_.find(identity.key);
        return it == decisions_.end() ? trust::Decision::unknown : it->second;
    }

    core::Result<void> approve_once(const trust::PhoneIdentity& identity) override {
        decisions_[identity.key] = trust::Decision::approved;
        ++approve_calls_;
        return {};
    }

    core::Result<void> forget(const trust::PhoneIdentity& identity) override {
        decisions_.erase(identity.key);
        ++forget_calls_;
        return {};
    }

    void set(const trust::PhoneIdentity& identity, trust::Decision decision) {
        decisions_[identity.key] = decision;
    }

    [[nodiscard]] int approve_calls() const noexcept { return approve_calls_; }
    [[nodiscard]] int forget_calls() const noexcept { return forget_calls_; }

private:
    std::map<std::string, trust::Decision> decisions_{};
    int approve_calls_{0};
    int forget_calls_{0};
};

class RecordingTransport final : public transport::Transport {
public:
    [[nodiscard]] transport::Kind kind() const noexcept override {
        return transport::Kind::usb_aoa;
    }

    core::Result<void> open(core::CancellationToken cancellation) override {
        bound_token_ = cancellation;
        ++open_calls_;
        if (on_open) {
            auto scripted = on_open();
            if (!scripted) {
                return scripted.error();
            }
        }
        is_open_ = true;
        return {};
    }

    core::Result<void> send(std::span<const std::byte>) override {
        return is_open_ ? core::Result<void>{}
                        : core::Result<void>{Error{ErrorCode::transport_closed}};
    }

    core::Result<std::vector<std::byte>> receive() override {
        return is_open_ ? core::Result<std::vector<std::byte>>{std::vector<std::byte>{}}
                        : core::Result<std::vector<std::byte>>{Error{ErrorCode::transport_closed}};
    }

    void close() noexcept override {
        is_open_ = false;
        ++close_calls_;
    }

    [[nodiscard]] core::CancellationToken bound_token() const noexcept {
        return bound_token_.value_or(core::CancellationToken{});
    }
    [[nodiscard]] int open_calls() const noexcept { return open_calls_; }
    [[nodiscard]] int close_calls() const noexcept { return close_calls_; }
    [[nodiscard]] bool is_open() const noexcept { return is_open_; }

    // Optional open-behavior hook: may advance the clock, request stop, or
    // return an error (a failed open that still owns resources).
    std::function<core::Result<void>()> on_open{};

private:
    std::optional<core::CancellationToken> bound_token_{};
    bool is_open_{false};
    int open_calls_{0};
    int close_calls_{0};
};

struct Harness {
    ManualClock clock{};
    MapTrustStore trust{};
    RecordingTransport transport{};
    RecordingTransport transport2{};
    Lifecycle lc;

    Harness() : Harness(LifecycleConfig{}) {}
    explicit Harness(LifecycleConfig config) : lc(config, clock, trust, &transport) {
        trust.set(kApprovedId, trust::Decision::approved);
        trust.set(kRejectedId, trust::Decision::rejected);
    }

    // Owner-thread replacement seam: supply a fresh transport for reconnect.
    void attach_fresh() { lc.attach_transport(&transport2); }

    [[nodiscard]] static bool ok(const core::Result<void>& result) {
        return result.has_value();
    }

    // Drives from a fresh (disconnected) controller to `target` along one
    // canonical legal path. Returns false if any step is unexpectedly refused.
    [[nodiscard]] bool drive_to(State target) {
        switch (target) {
        case State::disconnected:
            return true;
        case State::discovering:
            return ok(lc.apply(Event::start_discovery));
        case State::pairing:
            return ok(lc.apply(Event::start_discovery)) &&
                   ok(lc.authorize_pairing(kUnknownId));
        case State::connecting:
            return ok(lc.apply(Event::start_discovery)) && ok(lc.phone_discovered(kApprovedId));
        case State::negotiating:
            return drive_to(State::connecting) && ok(lc.apply(Event::transport_ready));
        case State::active:
            return drive_to(State::negotiating) && ok(lc.apply(Event::negotiation_succeeded));
        case State::degraded:
            return drive_to(State::active) && ok(lc.apply(Event::degrade));
        case State::reconnecting:
            return drive_to(State::active) && ok(lc.apply(Event::link_lost));
        case State::stopping:
            return drive_to(State::active) && stop_now();
        case State::failed:
            return drive_to(State::active) && ok(lc.fail(Error{ErrorCode::internal}));
        }
        return false;
    }

    [[nodiscard]] bool stop_now() {
        lc.request_stop();
        lc.tick();
        return lc.state() == State::stopping;
    }
};

} // namespace aa::session::test
