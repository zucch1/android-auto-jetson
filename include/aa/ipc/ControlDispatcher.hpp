// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/ControlService.hpp>
#include <aa/ipc/PeerCredentials.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace aa::ipc {

// D-Bus message kinds the dispatch boundary distinguishes (the wire codes of
// the type field). Only method_call may execute control semantics.
enum class WireType : std::uint8_t {
    other = 0,
    method_call = 1,
    method_return = 2,
    error_reply = 3,
    signal = 4,
};

// Narrow call-dispatch boundary (task 21) between raw bus traffic and the
// control surface. The caller identity is taken EXCLUSIVELY from the
// daemon-stamped unique sender of the actual incoming message — never from an
// argument — and message type, object path, interface, member and full
// in-signature are validated against the checked contract schema before any
// argument is interpreted. Only method-call messages to the published object
// path dispatch; signals and other traffic are dropped unexecuted, unknown
// contract majors (e.g. org.custom.AndroidAutoReceiver2) and malformed calls
// fail closed with a typed error reply.
//
// This is the receiver-side enforcement of "the D-Bus caller's Unix UID and
// registered consumer name" from the plan: every control method is routed
// here first, then to ControlService authorization with `sender` as caller.
struct IncomingCall final {
    BusName sender{};
    WireType type{WireType::other};
    std::string path{};
    std::string interface_name{};
    std::string member{};
    std::string signature{};
    std::vector<std::uint8_t> body{};
};

// The typed reply shapes of the checked contract: empty, t, s, (s t) state,
// or the a{ss} capability map. The wire adapter marshals this variant.
using ReplyBody =
    std::variant<std::monostate, std::uint64_t, std::string, StateSnapshot,
                 std::map<std::string, std::string>>;

struct OutboundReply final {
    bool is_error{false};
    std::string error_name{};
    ReplyBody body{};
};

class ControlDispatcher final {
public:
    explicit ControlDispatcher(ControlService& service) : service_(service) {}

    // Never throws and never trusts the caller. A method call yields a typed
    // method return or a typed D-Bus error reply; non-method-call traffic
    // (signals, returns, errors) yields no reply and is never dispatched.
    [[nodiscard]] std::optional<OutboundReply> handle(const IncomingCall& call) const;

private:
    ControlService& service_;
};

} // namespace aa::ipc
