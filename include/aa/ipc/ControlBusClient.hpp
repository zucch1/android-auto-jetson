// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/ControlService.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace aa::ipc {

// Client side of the REAL todo-21 contract (org.custom.AndroidAutoReceiver1)
// for an authorized test consumer (task 24's dummy display). Every projection
// control operation is a contract method call on the session bus; nothing
// here bypasses the contract. Authorization is satisfied exactly as the
// contract requires: the connection owns its consumer name on the bus before
// RegisterConsumer, and the caller's Unix UID must equal the active session
// user (same-UID processes qualify; the receiver enforces it).
//
// Threading: caller-driven; one thread at a time per instance. Declared
// contract signals are parked during method exchanges and drained by
// wait_signal(), so interleaved signals are never lost.
class ControlBusClient final {
public:
    // One declared contract signal as received on the wire (the five members
    // of the contract; per-member fields carry that member's arguments).
    struct ContractSignal final {
        std::string member{};
        ProjectionState projection{};
        core::ConsumerId consumer{};
        core::PairingRequestId request{};
        DiagnosticSeverity severity{};
        std::string event{};
        std::uint64_t subject{};
    };

    // Connects to the session bus (DBUS_SESSION_BUS_ADDRESS) and subscribes
    // to the contract's signals.
    [[nodiscard]] static core::Result<ControlBusClient> connect();

    ~ControlBusClient();
    ControlBusClient(const ControlBusClient&) = delete;
    ControlBusClient& operator=(const ControlBusClient&) = delete;
    ControlBusClient(ControlBusClient&& other) noexcept;
    ControlBusClient& operator=(ControlBusClient&& other) noexcept;

    // The receiver's daemon-stamped unique name; nullopt when no receiver
    // currently owns the contract name (the display shows "unavailable").
    [[nodiscard]] core::Result<std::optional<BusName>> receiver_owner();

    [[nodiscard]] core::Result<core::ConsumerId> register_consumer(const BusName& consumer_name);
    [[nodiscard]] core::Result<void> unregister_consumer();
    [[nodiscard]] core::Result<void> start_projection();
    [[nodiscard]] core::Result<void> stop_projection();
    [[nodiscard]] core::Result<void> set_display_viewport(Viewport viewport);
    [[nodiscard]] core::Result<StateSnapshot> get_state();
    [[nodiscard]] core::Result<std::uint64_t> ping();

    // Waits for the next declared contract signal (parked ones first). An
    // expired wait is the typed error `timeout`.
    [[nodiscard]] core::Result<ContractSignal> wait_signal(core::Milliseconds timeout);

    [[nodiscard]] const BusName& unique_name() const noexcept;

    struct Impl;

private:
    explicit ControlBusClient(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace aa::ipc
