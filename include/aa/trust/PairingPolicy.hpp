// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Approve-once pairing policy (task 27), wired to the task-21 one-time
// request-id gate through the PhoneDirectory seam.
//
// INVARIANT: the trust store changes ONLY inside pairing_confirmed, and only
// for the SAME one-time request id the PairingGate issued. RequestAddPhone
// (via the existing ControlService/PairingGate path) merely binds the phone
// identity that is currently presenting itself; nothing is persisted until a
// matching ConfirmPhonePairing lands inside the 60 s window. A request whose
// confirm does not arrive within the window is rejected and NOT persisted.
//
// Approve-once semantics: one approval grants reconnect eligibility (the
// store is the reconnect allowlist; known phones reconnect automatically via
// lookup); forget/revoke removes eligibility and persists the removal.
// Unknown phones fail closed — there is no code path that auto-approves.
//
// TRUST MODEL (see Identity.hpp): wired identity is physical-access trust,
// NOT cryptographic proof; USB identifier spoofing is a known, documented
// limitation of the wired trust boundary.
//
// Time: the policy reads an injected Clock so window expiry is deterministic
// and testable without sleeps. The clock seam lives here (not in aa::session)
// because trust is the lower module.

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/trust/Identity.hpp>
#include <aa/trust/Store.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace aa::trust {

// Narrow monotonic-time seam: production wraps steady_clock, tests drive a
// manual clock. Wall-clock time never enters a budget (core/Time contract).
class Clock {
public:
    virtual ~Clock() = default;
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    [[nodiscard]] virtual core::Nanoseconds now() const noexcept = 0;
};

class SteadyClock final : public Clock {
public:
    [[nodiscard]] core::Nanoseconds now() const noexcept override;
};

// The pairing policy IS the PhoneDirectory implementation behind
// ControlService::ForgetPhone and the task-21 pairing flow. It owns the
// pending-pairing table and the store mutations.
//
// Error contract (typed, fail closed):
//   - no candidate presented at pairing_requested -> trust_unknown_phone
//   - duplicate request binding                    -> trust_rejected
//   - pending table full                           -> ipc_queue_full
//   - confirm of an unbound/already-consumed id    -> trust_unknown_phone
//   - confirm past the 60 s window                 -> session_pairing_timeout
//     (the request is dropped and NOT persisted)
//   - store persistence failure                    -> trust_store_io
class PairingPolicy final : public ipc::PhoneDirectory {
public:
    static constexpr core::Milliseconds kConfirmWindow{60'000};

    PairingPolicy(ApprovedPhoneStore& store, const Clock& clock)
        : store_(store), clock_(clock) {}

    // Transport/session side: record the phone identity currently presenting
    // itself for approval (the discovered-but-unapproved phone). The identity
    // is bound at pairing_requested time so a later swap cannot change what an
    // in-flight confirm approves.
    void note_candidate(TransportIdentity identity);

    [[nodiscard]] const std::optional<TransportIdentity>& candidate() const noexcept {
        return candidate_;
    }

    // ipc::PhoneDirectory — driven by ControlService after the task-21 gate.
    [[nodiscard]] core::Result<void> forget(core::PhoneId phone) override;
    [[nodiscard]] core::Result<void> pairing_requested(core::PairingRequestId request) override;
    [[nodiscard]] core::Result<core::PhoneId> pairing_confirmed(
        core::PairingRequestId request) override;
    void pairing_cancelled(core::PairingRequestId request) override;

    // Drops pendings past the confirm window WITHOUT persisting anything and
    // returns the ids that expired. This is the ONLY expiry sweep: the ids are
    // propagated through the seam (never discarded internally) so the caller
    // can release each one's PairingGate entry and emit PairingClosed before
    // admitting new requests.
    [[nodiscard]] std::vector<core::PairingRequestId> reap_expired() override;

    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }
    [[nodiscard]] bool has_pending(core::PairingRequestId request) const;

    static constexpr std::size_t kMaxPending = 8;

private:
    struct Pending final {
        core::PairingRequestId request{};
        TransportIdentity identity{};
        core::Nanoseconds deadline{};
    };

    [[nodiscard]] std::size_t find(core::PairingRequestId request) const;
    void drop_at(std::size_t index);

    ApprovedPhoneStore& store_;
    const Clock& clock_;
    std::optional<TransportIdentity> candidate_{};
    std::vector<Pending> pending_{};
};

} // namespace aa::trust
