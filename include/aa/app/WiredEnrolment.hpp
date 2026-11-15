// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wired enrolment (#133): the receiver-side state machine that lets the owner
// approve a wired phone the store refused, through the authenticated D-Bus
// control contract. It is the ipc::PhoneDirectory the wired receiver hands
// ControlService while no session is running, and it follows the
// RequestAddPhone / PairingGate shape so a consumer UI drives wired and
// wireless enrolment the same way:
//
//   1. admission refuses an unknown phone (transport_unauthorized_peer) and
//      the receiver records that attempt's CAPTURED identity: note_refused().
//      This is the only way a wired candidate enters; an admitted link, AOA
//      success, or a phone that was never refused is never a candidate.
//   2. the registered consumer calls RequestAddPhone. The one-time request id
//      is bound to a snapshot of the refused identity (PairingPolicy
//      SNAPSHOT-ONLY contract) if that refusal is fresh (kCandidateFreshness).
//   3. the consumer calls GetPairingCandidate and shows the redacted label to
//      a human, who answers yes (ConfirmPhonePairing) or no
//      (CancelPhonePairing). A confirm for a candidate that was never
//      retrieved is refused and the request closes (fail closed: no approval
//      of an unseen phone).
//   4. a yes inside the 60 s PairingPolicy window writes the bound identity
//      to the store durably (ApprovedPhoneStore::approve). Expiry, a no,
//      consumer teardown or close() write nothing.
//
// Serial-less (port-only) identities are refused at RequestAddPhone with
// trust_port_only_requires_opt_in (PairingPolicy): this flow never grants the
// weaker port-only approval. Wireless candidates are not handled here.
//
// close() ends enrolment (a session is about to own the store from another
// thread): later requests are refused, the next reap_expired() hands every
// outstanding request back so ControlService releases its gate entry and emits
// PairingClosed, and forget/revocation delegate to `after`.
//
// Ownership/threading: the store-owner thread (the thread that runs the bus
// dispatch while no session is active) calls everything, serialized, exactly
// like PairingPolicy. No internal locks.

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/trust/Identity.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <aa/trust/Store.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aa::app {

class WiredEnrolment final : public ipc::PhoneDirectory {
public:
    // A refusal older than this is stale (the phone may have been unplugged),
    // so RequestAddPhone binds nothing. The receiver holds enrolment open for
    // the same window after a refusal and then stops (exit 3, as before).
    static constexpr core::Milliseconds kCandidateFreshness{120'000};

    WiredEnrolment(trust::ApprovedPhoneStore& store, const trust::Clock& clock,
                   ipc::PhoneDirectory& after)
        : clock_(clock), policy_(store, clock), after_(after) {}

    // Record the captured identity of an attempt admission REFUSED. Fails
    // invalid_argument for an identity with no usable material; fails
    // cancelled once closed. Replacing the candidate never changes what an
    // already-bound request approves.
    [[nodiscard]] core::Result<void> note_refused(const trust::WiredIdentity& identity);

    void close();
    [[nodiscard]] bool open() const noexcept { return open_; }
    [[nodiscard]] std::size_t pending_count() const noexcept { return requests_.size(); }

    // ipc::PhoneDirectory
    [[nodiscard]] core::Result<void> forget(core::PhoneId phone) override;
    std::optional<helper::RevocationV1> take_revocation_v1() override;
    [[nodiscard]] core::Result<void> pairing_requested(core::PairingRequestId request) override;
    [[nodiscard]] core::Result<core::PhoneId> pairing_confirmed(core::PairingRequestId request) override;
    core::Result<void> validate_pairing(core::PairingRequestId request) override;
    std::optional<core::PhoneId> committed_pairing() const override;
    void pairing_cancelled(core::PairingRequestId request) override;
    [[nodiscard]] std::vector<core::PairingRequestId> reap_expired() override;
    [[nodiscard]] std::vector<core::PairingRequestId> take_invalidated_pairings() override;
    std::optional<std::map<std::string, std::string>> pairing_candidate(core::PairingRequestId request) override;

private:
    struct Request final {
        core::PairingRequestId id{};
        trust::TransportIdentity identity{};
        bool presented{false};
    };
    [[nodiscard]] Request* find(core::PairingRequestId request);
    void untrack(core::PairingRequestId request);

    const trust::Clock& clock_;
    trust::PairingPolicy policy_;
    ipc::PhoneDirectory& after_;
    std::optional<trust::WiredIdentity> refused_{};
    core::Nanoseconds refused_at_{};
    std::vector<Request> requests_{};
    std::vector<core::PairingRequestId> invalidated_{};
    bool open_{true};
};

} // namespace aa::app
