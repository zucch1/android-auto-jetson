// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/helper/Confirmation.hpp>
#include <aa/core/Result.hpp>
#include <aa/ipc/Control.hpp>
#include <aa/ipc/PairingGate.hpp>
#include <aa/ipc/PeerCredentials.hpp>

#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aa::ipc {

// Contract projection state (GetState / StateChanged). This is the control
// surface's own state; the ten-state session machine (aa/session) is driven by
// the session owner and is not duplicated here.
enum class ProjectionState { stopped, projecting };

[[nodiscard]] constexpr std::string_view to_string(ProjectionState state) noexcept {
    switch (state) {
    case ProjectionState::stopped: return "stopped";
    case ProjectionState::projecting: return "projecting";
    }
    return "unknown";
}

struct StateSnapshot final {
    ProjectionState projection{ProjectionState::stopped};
    core::ConsumerId consumer{};

    [[nodiscard]] friend bool operator==(const StateSnapshot&, const StateSnapshot&) noexcept =
        default;
};

enum class DiagnosticSeverity { info, warning, error };

[[nodiscard]] constexpr std::string_view to_string(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::info: return "info";
    case DiagnosticSeverity::warning: return "warning";
    case DiagnosticSeverity::error: return "error";
    }
    return "unknown";
}

// Typed control-surface occurrences, one per D-Bus signal in the contract.
// The wire adapter maps these to PairingRequest/PairingOpened/PairingClosed/
// StateChanged/DiagnosticEvent; nothing here carries audio, video or any other
// high-rate payload.
struct StateChanged final {
    ProjectionState projection{};
    core::ConsumerId consumer{};
};
struct PairingRequested final {
    core::PairingRequestId request{};
};
struct PairingOpened final {
    core::PairingRequestId request{};
};
struct PairingClosed final {
    core::PairingRequestId request{};
};
struct DiagnosticEmitted final {
    DiagnosticSeverity severity{};
    std::string_view event{};  // stable literal name, never dynamic data
    std::uint64_t subject{};
};

using ControlEvent =
    std::variant<StateChanged, PairingRequested, PairingOpened, PairingClosed, DiagnosticEmitted>;

class ControlEventSink {
public:
    virtual ~ControlEventSink() = default;
    ControlEventSink() = default;
    ControlEventSink(const ControlEventSink&) = delete;
    ControlEventSink& operator=(const ControlEventSink&) = delete;
    ControlEventSink(ControlEventSink&&) = delete;
    ControlEventSink& operator=(ControlEventSink&&) = delete;

    virtual void emit(const ControlEvent& event) = 0;
};

// ForgetPhone seam: the trust-store binding (phone id -> identity erase) is
// todo 27 and plugs in here; the control surface owns only the authorization
// gate and typed id validation around it.
// Trust-store seam (task 21 forget path, task 27 approval flow). The control
// surface owns only the authorization gate, the one-time request-id gate and
// typed id validation; every trust-store mutation is forwarded here so the
// store changes ONLY after ConfirmPhonePairing for the SAME one-time request
// id. Typed minimal extension over the task-21 forget-only seam (task 27):
//   - pairing_requested  : a request id was issued; bind the presented phone
//     identity and start the pairing window. Rejects (and nothing is written)
//     when no phone identity is presenting itself.
//   - pairing_confirmed  : the same request id was confirmed within the
//     window; approve-once and persist. Late confirms are rejected and NOT
//     persisted.
//   - pairing_cancelled  : request/window closed without any store change.
//   - reap_expired       : return (and drop) request ids whose pairing window
//     just expired WITHOUT persisting anything. The caller must release the
//     gate entries for the returned ids and emit exactly one PairingClosed
//     each; expired ids are propagated here so the one-time gate can never
//     keep a dead pairing request consuming admission capacity.
//   - take_invalidated_pairings : drain the request ids whose pending pairing
//     was invalidated by a successful (or committed-but-not-durable) forget.
//     The caller must release each id's gate entry and emit exactly one
//     PairingClosed BEFORE returning the forget result; the drain clears the
//     buffer. A pre-replacement forget failure invalidates nothing.
class PhoneDirectory {
public:
    virtual ~PhoneDirectory() = default;
    PhoneDirectory() = default;
    PhoneDirectory(const PhoneDirectory&) = delete;
    PhoneDirectory& operator=(const PhoneDirectory&) = delete;
    PhoneDirectory(PhoneDirectory&&) = delete;
    PhoneDirectory& operator=(PhoneDirectory&&) = delete;

    [[nodiscard]] virtual core::Result<void> forget(core::PhoneId phone) = 0;
    virtual std::optional<helper::RevocationV1> take_revocation_v1() { return std::nullopt; }
    [[nodiscard]] virtual core::Result<void> pairing_requested(
        core::PairingRequestId request) = 0;
    [[nodiscard]] virtual core::Result<core::PhoneId> pairing_confirmed(
        core::PairingRequestId request) = 0;
    // Validate the bound request and expire it without persisting. Called
    // before privileged confirmation; pairing_confirmed rechecks after RPC.
    virtual core::Result<void> validate_pairing(core::PairingRequestId) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    // Populated only by the last committed-but-not-durable confirmation.
    virtual std::optional<core::PhoneId> committed_pairing() const { return std::nullopt; }
    virtual void pairing_cancelled(core::PairingRequestId request) = 0;
    [[nodiscard]] virtual std::vector<core::PairingRequestId> reap_expired() = 0;
    [[nodiscard]] virtual std::vector<core::PairingRequestId> take_invalidated_pairings() = 0;
    virtual core::Result<void> note_wireless_candidate(const helper::PeerIdentity&) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    virtual bool same_wireless_identity(const helper::PeerIdentity& a,
                                        const helper::PeerIdentity& b) const { return a == b; }
    virtual std::optional<helper::PeerIdentity> wireless_identity(core::PhoneId) const { return std::nullopt; }
    // Every approved wireless identity (the bond authority sent to the helper's reconciliation);
    // nullopt when the store is unusable, so nothing is reconciled and the helper stays fail-closed.
    virtual std::optional<std::vector<helper::PeerIdentity>> approved_wireless() const { return std::nullopt; }
    virtual std::optional<helper::PeerIdentity> pairing_wireless_identity(core::PairingRequestId) const { return std::nullopt; }
    virtual std::optional<helper::PeerIdentity> take_expired_wireless_identity(core::PairingRequestId) {
        return std::nullopt;
    }
};

// The active session user the caller's Unix UID must equal. Production fills
// this from the receiver's own session user; tests set it explicitly.
struct ControlServiceConfig final {
    std::uint32_t session_unix_uid{};
};

struct ControlServiceDeps final {
    PeerCredentialLookup& credentials;
    ControlEventSink& events;
    PairingIdSource& pairing_ids;
    PhoneDirectory& phones;
    helper::Confirmation* confirmation{nullptr};
    std::function<std::chrono::steady_clock::time_point()> clock{};
};

// Session D-Bus control dispatcher (task 21) for
// org.custom.AndroidAutoReceiver1: the fourteen contract methods with their
// authorization semantics, the one-time pairing request-id gate, and the
// typed event stream behind the contract's signals.
//
// Ownership/threading: one instance per receiver, driven from the session
// thread (the future D-Bus adapter parses wire arguments at the boundary and
// posts here). Every method takes the caller's D-Bus connection name exactly
// as the bus delivered it; authorization always fails closed.
//
// Admission (single active session consumer): only the registered consumer —
// caller Unix UID equal to the active session user AND caller owning the
// registered consumer name — may drive projection control or pairing. Read
// status (state, capabilities, ping) stays open to every caller.
class ControlService final {
public:
    static constexpr std::size_t kSourceRetirementAttemptBudget = 4;
    static constexpr std::chrono::milliseconds kSourceRetirementTimeBudget{500};

    ControlService(ControlServiceConfig config, ControlServiceDeps deps)
        : config_(config), credentials_(deps.credentials), events_(deps.events),
          phones_(deps.phones), gate_(deps.pairing_ids), confirmation_(deps.confirmation),
          clock_(std::move(deps.clock)) {}

    // RegisterConsumer / UnregisterConsumer.
    [[nodiscard]] core::Result<core::ConsumerId> register_consumer(const BusName& caller,
                                                                   const BusName& consumer_name);
    [[nodiscard]] core::Result<void> unregister_consumer(const BusName& caller);

    // Lookup-on-use owner reconciliation (the minimal POSIX bus client has no
    // event loop, so there is no NameOwnerChanged monitoring): if a consumer is
    // registered, re-resolve its owned name through the credential lookup and
    // compare against the stored UNIQUE connection. Absent, different or
    // unverifiable ownership all revoke authority and tear the registration
    // down (lookup failures close conservatively — the production lookup
    // collapses every bus failure to ipc_peer_unauthorized, so unverifiable
    // == revoked). With no registration this is a no-op. The future runtime
    // (task 24) MUST call this while idle: lookup-on-use cannot detect an idle
    // disconnect on its own.
    void refresh_consumer_owner();
    void shutdown() {
        if (registered_) teardown("consumer_unregistered", DiagnosticSeverity::info);
    }

    // StartProjection / StopProjection.
    [[nodiscard]] core::Result<void> start_projection(const BusName& caller);
    [[nodiscard]] core::Result<void> stop_projection(const BusName& caller);

    // Pairing flow: one-time request ids, window opens only on confirm.
    [[nodiscard]] core::Result<core::PairingRequestId> request_add_phone(const BusName& caller);
    [[nodiscard]] core::Result<void> confirm_phone_pairing(const BusName& caller,
                                                           core::PairingRequestId request);
    [[nodiscard]] core::Result<void> cancel_phone_pairing(const BusName& caller,
                                                          core::PairingRequestId request);
    [[nodiscard]] core::Result<void> forget_phone(const BusName& caller, core::PhoneId phone);
    core::Result<std::map<std::string, std::string>> pairing_comparison(const BusName&, core::PairingRequestId);
    core::Result<void> respond_pairing_comparison(const BusName&, core::PairingRequestId, helper::ComparisonResponse);

    // GetState / GetCapabilities / Ping (open to every caller). state()
    // reconciles the incumbent owner before snapshotting and can emit the
    // teardown events, so it is not const or noexcept.
    [[nodiscard]] StateSnapshot state();
    [[nodiscard]] std::map<std::string, std::string> capabilities() const;
    [[nodiscard]] std::uint64_t ping() const noexcept;

    // SetDisplayViewport (consumer-only).
    [[nodiscard]] core::Result<void> set_display_viewport(const BusName& caller, Viewport viewport);
    // Cached projection viewport: the value set by the consumer-owned
    // SetDisplayViewport call; cleared when the consumer registration ends.
    [[nodiscard]] std::optional<Viewport> viewport() const { return viewport_; }

    // Pairing-window observability (the todo-27 machinery hooks this). Cached:
    // reflects the gate state as of the last control call.
    [[nodiscard]] bool pairing_window_open() const noexcept { return gate_.window_open(); }
    std::size_t pending_revocations() const noexcept { return revocations_.size(); }
    std::optional<helper::RevocationProgress> revocation_progress(core::PhoneId phone) const;

private:
    struct Registered final {
        core::ConsumerId consumer{};
        BusName name{};
        BusName connection{};
        std::uint32_t unix_uid{};
    };

    // Shared teardown for unregister and observed owner loss: projection goes
    // stopped (preserving that stopped event for the old consumer), every
    // pending/open pairing request closes exactly once, then registration and
    // the consumer-owned viewport clear behind a final cleared StateChanged and
    // the reason diagnostic ("consumer_unregistered" or "consumer_owner_lost").
    // The PairingGate instance and its issued-id history are PRESERVED: a
    // cycling id source can never reissue a spent id after teardown.
    void teardown(std::string_view reason, DiagnosticSeverity severity);

    [[nodiscard]] core::Result<Registered> require_consumer();
    [[nodiscard]] core::Result<void> admit(const BusName& caller, const Registered& active);
    // durable (optional out): false when the approval landed only as a committed-but-not-durable
    // store result (703).
    core::Result<void> persist_pairing(const Registered&, core::PairingRequestId, bool* durable = nullptr);
    // Revokes an approved phone: store forget plus tracked external revocation (ForgetPhone's core).
    core::Result<void> revoke_phone(const Registered&, core::PhoneId);
    void close_enrolment(std::string_view reason, bool bond_recorded = false);
    // Bond reconciliation (docs/enrolment-ordering.md). Paced from refresh_consumer_owner(): due at
    // start, after any helper failure and whenever an enrolment ends without a commit; otherwise a
    // cheap owner/set check per second that reconciles a reappeared helper. A success resolves
    // every revocation record's bond leg whose identity the store no longer approves.
    void reconcile_bonds();
    void request_reconcile() { reconcile_due_ = true; next_reconcile_ = std::chrono::steady_clock::time_point::min(); }
    bool reconcile_enrolment(core::PairingRequestId, bool bond_recorded);
    void advance_enrolment();
    struct PendingRevocation {
        helper::RevocationV1 snapshot;
        helper::RevocationProgress progress;
        bool not_durable;
        std::uint32_t failures{0};
        std::chrono::steady_clock::time_point next_attempt{
            std::chrono::steady_clock::time_point::min()};
    };
    [[nodiscard]] std::chrono::steady_clock::time_point now() const;
    // A numeric-comparison bond the helper may already manage whose enrolment did not complete:
    // the existing revocation record (bond leg + source retirement) removes it and blocks the
    // identity until acknowledged. It is retried by the same bounded loop as source-only records.
    void retire_acknowledged_pairing(core::ConsumerId, core::PairingRequestId,
                                     const std::optional<helper::PeerIdentity>&);
    core::Result<void> attempt_revocation(PendingRevocation& record);
    void retire_failed_pairing(core::ConsumerId, core::PairingRequestId,
                               const std::optional<helper::PeerIdentity>&);
    bool revocation_blocks(const helper::PeerIdentity&) const;

    ControlServiceConfig config_{};
    PeerCredentialLookup& credentials_;
    ControlEventSink& events_;
    PhoneDirectory& phones_;
    PairingGate gate_;
    helper::Confirmation* confirmation_{};
    std::function<std::chrono::steady_clock::time_point()> clock_{};
    std::vector<PendingRevocation> revocations_;
    std::size_t source_retry_cursor_{0};
    bool reconcile_due_{true};
    bool reconcile_failing_{};
    std::uint64_t reconcile_unadmitted_{};
    std::chrono::steady_clock::time_point next_reconcile_{std::chrono::steady_clock::time_point::min()};
    std::optional<Registered> registered_{};
    ProjectionState projection_{ProjectionState::stopped};
    std::optional<Viewport> viewport_{};
    std::uint64_t next_consumer_id_{1};
    struct OpenPairing {
        core::PairingRequestId request;
        core::PhoneId phone;
        std::optional<helper::PeerIdentity> peer;
        std::chrono::steady_clock::time_point deadline;
        std::optional<std::uint64_t> presented_comparison{};
        bool awaiting_acceptance{};
        bool accepted{};
    };
    std::optional<OpenPairing> open_pairing_;
};

} // namespace aa::ipc
