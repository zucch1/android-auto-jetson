// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/ipc/Control.hpp>
#include <aa/ipc/PairingGate.hpp>
#include <aa/ipc/PeerCredentials.hpp>

#include <cstdint>
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
class PhoneDirectory {
public:
    virtual ~PhoneDirectory() = default;
    PhoneDirectory() = default;
    PhoneDirectory(const PhoneDirectory&) = delete;
    PhoneDirectory& operator=(const PhoneDirectory&) = delete;
    PhoneDirectory(PhoneDirectory&&) = delete;
    PhoneDirectory& operator=(PhoneDirectory&&) = delete;

    [[nodiscard]] virtual core::Result<void> forget(core::PhoneId phone) = 0;
    [[nodiscard]] virtual core::Result<void> pairing_requested(
        core::PairingRequestId request) = 0;
    [[nodiscard]] virtual core::Result<core::PhoneId> pairing_confirmed(
        core::PairingRequestId request) = 0;
    virtual void pairing_cancelled(core::PairingRequestId request) = 0;
    [[nodiscard]] virtual std::vector<core::PairingRequestId> reap_expired() = 0;
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
};

// Session D-Bus control dispatcher (task 21) for
// org.custom.AndroidAutoReceiver1: the twelve contract methods with their
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
    ControlService(ControlServiceConfig config, ControlServiceDeps deps)
        : config_(config), credentials_(deps.credentials), events_(deps.events),
          phones_(deps.phones), gate_(deps.pairing_ids) {}

    // RegisterConsumer / UnregisterConsumer.
    [[nodiscard]] core::Result<core::ConsumerId> register_consumer(const BusName& caller,
                                                                   const BusName& consumer_name);
    [[nodiscard]] core::Result<void> unregister_consumer(const BusName& caller);

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

    // GetState / GetCapabilities / Ping (open to every caller).
    [[nodiscard]] StateSnapshot state() const noexcept;
    [[nodiscard]] std::map<std::string, std::string> capabilities() const;
    [[nodiscard]] std::uint64_t ping() const noexcept;

    // SetDisplayViewport (consumer-only).
    [[nodiscard]] core::Result<void> set_display_viewport(const BusName& caller, Viewport viewport);
    [[nodiscard]] std::optional<Viewport> viewport() const { return viewport_; }

    // Pairing-window observability (the todo-27 machinery hooks this).
    [[nodiscard]] bool pairing_window_open() const noexcept { return gate_.window_open(); }

private:
    struct Registered final {
        core::ConsumerId consumer{};
        BusName name{};
        BusName connection{};
        std::uint32_t unix_uid{};
    };

    [[nodiscard]] core::Result<Registered> require_consumer() const;
    [[nodiscard]] core::Result<void> admit(const BusName& caller, const Registered& active) const;

    ControlServiceConfig config_{};
    PeerCredentialLookup& credentials_;
    ControlEventSink& events_;
    PhoneDirectory& phones_;
    PairingGate gate_;
    std::optional<Registered> registered_{};
    ProjectionState projection_{ProjectionState::stopped};
    std::optional<Viewport> viewport_{};
    std::uint64_t next_consumer_id_{1};
};

} // namespace aa::ipc
