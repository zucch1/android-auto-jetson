// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/ControlService.hpp>

#include <cstdint>
#include <memory>

namespace aa::ipc {

// Live D-Bus service export (task-21 integration obligation landed with task
// 24): owns the documented receiver name (kContractBusName) and object path
// (kContractObjectPath) on the session bus, runs continuous dispatch through
// ControlDispatcher/ControlService (the authorization boundary and teardown
// logic are reused, never bypassed or weakened), and emits the declared
// contract events as REAL D-Bus signals (PairingRequest, PairingOpened,
// PairingClosed, StateChanged, DiagnosticEvent). Audio/video payloads never
// cross this surface.
//
// Idle owner cleanup: cycle() polls the bus with a caller-bounded timeout and
// treats an expired window with no traffic as "runtime idle"; the registered
// consumer's owner is then reconciled through
// ControlService::refresh_consumer_owner() exactly per its documented
// contract (lookup failures close conservatively). The loop passes the idle
// interval as the cycle timeout; kIdleOwnerRefreshInterval (1 s) is the
// recommended value and bounds owner-loss detection for an idle consumer to
// one interval.
//
// Threading: caller-driven and thread-free like every adapter here. Exactly
// one thread runs cycle() at a time; the enclosed ControlService is confined
// to that thread.
class ControlBusService final {
public:
    static constexpr core::Milliseconds kIdleOwnerRefreshInterval{1000};

    struct Config final {
        std::uint32_t session_unix_uid{};
    };

    // Seams whose policy stays caller-owned (pairing id allocation and the
    // todo-27 phone directory plug-in).
    struct Deps final {
        PairingIdSource& pairing_ids;
        PhoneDirectory& phones;
        helper::Confirmation* confirmation{nullptr};
    };

    // Connects to the session bus (DBUS_SESSION_BUS_ADDRESS), owns the
    // documented receiver name and returns the running export. transport_io /
    // transport_unavailable when the bus or the name cannot be taken.
    [[nodiscard]] static core::Result<ControlBusService> start(Config config, Deps deps);

    ~ControlBusService();
    ControlBusService(const ControlBusService&) = delete;
    ControlBusService& operator=(const ControlBusService&) = delete;
    ControlBusService(ControlBusService&& other) noexcept;
    ControlBusService& operator=(ControlBusService&& other) noexcept;

    // One dispatch step: waits up to `idle_interval` for one incoming message
    // and answers method calls through the dispatch boundary; a window that
    // expires with no traffic reconciles the consumer owner (idle cleanup) and
    // counts one idle_refresh(). Non-method traffic is dropped unexecuted.
    // Returns the number of messages dispatched. timeout / transport errors
    // are typed; a closed export yields transport_closed.
    [[nodiscard]] core::Result<std::size_t> cycle(core::Milliseconds idle_interval);

    // Releases the bus name and connection; idempotent.
    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] const BusName& unique_name() const noexcept;
    [[nodiscard]] std::uint64_t idle_refreshes() const noexcept;

    // The authorization boundary the export dispatches into (session-thread
    // confined; exposed for teardown/runtime integration, never bypassed).
    [[nodiscard]] ControlService& service() noexcept;

    struct Impl;

private:
    explicit ControlBusService(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace aa::ipc
