// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Error.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/session/Clock.hpp>
#include <aa/session/Events.hpp>
#include <aa/session/Session.hpp>
#include <aa/transport/Transport.hpp>
#include <aa/trust/Trust.hpp>

#include <initializer_list>
#include <optional>

namespace aa::session {

// Caller-supplied phase budgets. These are NOT invented contract budgets: zero
// means the caller did not bound that phase and the controller arms no deadline
// for it. The mandated contract budgets (pairing 60 s, reconnect 5 s/15 s,
// consumer grace 10 s) are fixed inside the controller and never overridable.
struct PhaseBudget final {
    core::Milliseconds discovery{0};
    core::Milliseconds connect{0};
    core::Milliseconds negotiate{0};
    core::Milliseconds stop{0};
};

// Lifecycle configuration: caller phase budgets plus the link medium that
// selects the contract reconnect budget.
struct LifecycleConfig final {
    PhaseBudget phases{};
    LinkKind link{LinkKind::wired};
};

// Session-thread-owned lifecycle policy controller (task 17). Bounded lifecycle
// over all ten states with an explicit transition policy that is stricter than
// the raw StateMachine table: fail-closed trust admission, an explicit pairing
// intent gate, deterministic injected monotonic deadlines, and a
// CancellationSource shared with an attached Transport.
//
// Threading: owned and mutated only on the session thread
// (aa::core::SessionThread). request_stop() is the sole cross-thread entry and
// only fires the cancellation token; the resulting stop transition and all
// transport open/close run later on the owner thread (via tick / enter), so
// pending transport I/O is never closed concurrently.
//
// Admission: only a trust::Decision::approved phone may reach connecting /
// negotiating / active. Discovery is never approval. Pairing requires an explicit
// authorized intent; an external approval is re-confirmed via TrustStore lookup
// before connecting. Unknown and rejected phones fail closed on every path.
//
// Transport open: a synchronous transport open is expected to complete within a
// 10 s upper bound. Task 17 has no realtime scheduler to interrupt it, so the
// connect decision is armed before the open and caller phase budgets (and any
// armed reconnect budget) are enforced once the open returns.
class Lifecycle final {
public:
    Lifecycle(LifecycleConfig config, MonotonicClock& clock,
              const trust::TrustStore& trust, transport::Transport* transport);
    ~Lifecycle() = default;

    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;
    Lifecycle(Lifecycle&&) = delete;
    Lifecycle& operator=(Lifecycle&&) = delete;

    [[nodiscard]] State state() const noexcept { return machine_.state(); }

    // The typed reason the session last entered failed (e.g. pairing timeout).
    [[nodiscard]] std::optional<Error> failure() const { return last_failure_; }

    // Token shared with the attached Transport (open binds it). Cross-thread
    // request_stop() fires this same token.
    [[nodiscard]] core::CancellationToken token() const noexcept {
        return cancel_.get_token();
    }

    // Cross-thread cancellation: fires the token only. The stop transition and
    // cleanup happen on the owner thread when tick() observes it.
    void request_stop() noexcept;

    // Owner-thread observation point: drives cancellation and deadline expiry
    // before the next transition. At most one transition per call.
    void tick();

    // Payload-free lifecycle events (see Events.hpp). Illegal-in-state returns
    // session_illegal_transition and leaves the state unchanged.
    core::Result<void> apply(Event event);

    // Phone-identity events. Each re-decides admission from the TrustStore.
    core::Result<void> phone_discovered(const trust::PhoneIdentity& identity);
    core::Result<void> authorize_pairing(const trust::PhoneIdentity& identity);
    core::Result<void> confirm_pairing(const trust::PhoneIdentity& identity);

    // Typed failure transition into failed. Records `reason` as the failure().
    core::Result<void> fail(Error reason);

    // Owner-thread transport replacement seam. A real transport is terminal once
    // its link drops, so a reconnect supplies a fresh handle here; the controller
    // never reuses a retired one. Only call from the owner thread.
    void attach_transport(transport::Transport* transport) noexcept;

private:
    [[nodiscard]] core::Nanoseconds now() const noexcept;
    [[nodiscard]] core::Result<void> guard() const;
    [[nodiscard]] bool in_state(std::initializer_list<State> allowed) const noexcept;
    [[nodiscard]] core::Result<void> transition_if(std::initializer_list<State> allowed,
                                                   State to);
    [[nodiscard]] core::Result<void> admit(const trust::PhoneIdentity& identity);
    [[nodiscard]] core::Result<void> readmit();
    [[nodiscard]] core::Result<void> begin_transport();
    [[nodiscard]] core::Result<void> begin_step(bool progress);
    [[nodiscard]] core::Result<void> post_open_check();
    [[nodiscard]] core::Result<void> complete_connect();
    [[nodiscard]] core::Result<void> enter_failed(Error reason);
    [[nodiscard]] std::optional<ErrorCode> observe_deadlines();
    [[nodiscard]] core::Result<State> enter(State to);
    void arm_deadline(std::optional<core::Nanoseconds>& slot, core::Milliseconds budget,
                      core::Nanoseconds at);
    void clear_provisional_pairing() noexcept;
    void retire_transport() noexcept;
    void run_cleanup() noexcept;

    LifecycleConfig config_;
    MonotonicClock& clock_;
    const trust::TrustStore& trust_;
    transport::Transport* transport_; // attached; owned elsewhere (may be null)
    core::CancellationSource cancel_;
    StateMachine machine_;

    std::optional<trust::PhoneIdentity> phone_identity_;  // admitted phone
    std::optional<trust::PhoneIdentity> pairing_identity_; // provisional pairing
    bool pairing_authorized_{false};
    bool transport_opened_{false};
    bool transport_retired_{false};

    std::optional<core::Nanoseconds> pairing_deadline_;
    std::optional<core::Nanoseconds> reconnect_deadline_;
    std::optional<core::Nanoseconds> phase_deadline_;
    std::optional<core::Nanoseconds> grace_deadline_;
    std::optional<Error> last_failure_;
};

} // namespace aa::session
