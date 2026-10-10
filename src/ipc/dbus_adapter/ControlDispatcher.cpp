// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlDispatcher.hpp>

#include <aa/ipc/Contract.hpp>
#include <aa/ipc/dbus_contract.generated.hpp>

#include "BusWire.hpp"
#include "DispatchReply.hpp"
#include "PropertiesDispatch.hpp"

#include <cstddef>

namespace aa::ipc {

namespace {

constexpr std::string_view kIntrospectableInterface = "org.freedesktop.DBus.Introspectable";
constexpr std::string_view kPropertiesInterface = "org.freedesktop.DBus.Properties";

[[nodiscard]] std::string_view error_name_for(ErrorCode code) {
    switch (code) {
    case ErrorCode::ipc_peer_unauthorized: return kPeerUnauthorizedError;
    case ErrorCode::ipc_consumer_rejected: return kConsumerRejectedError;
    case ErrorCode::ipc_malformed_request: return kMalformedRequestError;
    case ErrorCode::ipc_queue_full: return kQueueFullError;
    case ErrorCode::session_illegal_transition: return kIllegalTransitionError;
    case ErrorCode::invalid_argument: return "org.freedesktop.DBus.Error.InvalidArgs";
    default: return "org.freedesktop.DBus.Error.Failed";
    }
}

[[nodiscard]] OutboundReply from_void(const core::Result<void>& result) {
    return result.has_value() ? ok(std::monostate{})
                              : error_reply(error_name_for(result.error().code()),
                                            result.error().message());
}

[[nodiscard]] bool is_unique_name(std::string_view name) {
    if (name.size() < 2 || name.front() != ':') {
        return false;
    }
    bool has_digit = false;
    for (const char character : name.substr(1)) {
        if (character == '.') {
            continue;
        }
        if (character < '0' || character > '9') {
            return false;
        }
        has_digit = true;
    }
    return has_digit;
}

[[nodiscard]] OutboundReply dispatch_member(ControlService& service, const BusName& sender,
                                            std::size_t index, std::span<const std::uint8_t> body) {
    dbus_adapter::Cursor cursor(body);
    switch (index) {
    case 0: {
        auto name = cursor.read_string();
        if (!name.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        const auto registered = service.register_consumer(sender, BusName{name.value()});
        if (!registered.has_value()) {
            return error_reply(error_name_for(registered.error().code()),
                               registered.error().message());
        }
        return ok(registered.value().value);
    }
    case 1: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.unregister_consumer(sender));
    }
    case 2: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.start_projection(sender));
    }
    case 3: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.stop_projection(sender));
    }
    case 4: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        const auto request = service.request_add_phone(sender);
        if (!request.has_value()) {
            return error_reply(error_name_for(request.error().code()), request.error().message());
        }
        return ok(request.value().value);
    }
    case 5: {
        auto id = cursor.read_u64();
        if (!id.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(
            service.confirm_phone_pairing(sender, core::PairingRequestId{id.value()}));
    }
    case 6: {
        auto id = cursor.read_u64();
        if (!id.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.cancel_phone_pairing(sender, core::PairingRequestId{id.value()}));
    }
    case 7: {
        auto id = cursor.read_u64();
        if (!id.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.forget_phone(sender, core::PhoneId{id.value()}));
    }
    case 8: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return ok(service.state());
    }
    case 9: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return ok(service.capabilities());
    }
    case 10: {
        auto width = cursor.read_u32();
        auto height = cursor.read_u32();
        auto dpi = cursor.read_u32();
        if (!width.has_value() || !height.has_value() || !dpi.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        if (width.value() > 65535U || height.value() > 65535U) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "viewport out of range");
        }
        const Viewport viewport{static_cast<std::uint16_t>(width.value()),
                                static_cast<std::uint16_t>(height.value()), dpi.value()};
        return from_void(service.set_display_viewport(sender, viewport));
    }
    default: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return ok(service.ping());
    }
    }
}

} // namespace

std::optional<OutboundReply> ControlDispatcher::handle(const IncomingCall& call) const {
    if (call.type != WireType::method_call) {
        return std::nullopt;
    }
    if (call.path != kContractObjectPath) {
        return error_reply("org.freedesktop.DBus.Error.UnknownObject", "unknown object path");
    }
    if (!is_unique_name(call.sender.value)) {
        return error_reply("org.freedesktop.DBus.Error.Failed", "missing daemon-stamped sender");
    }
    if (call.interface_name == kIntrospectableInterface) {
        if (call.member != "Introspect" || !call.signature.empty() || !call.body.empty()) {
            return error_reply("org.freedesktop.DBus.Error.UnknownMethod", "unknown member");
        }
        return ok(std::string{introspection_xml()});
    }
    if (call.interface_name == kPropertiesInterface) {
        return dispatch_properties(call);
    }
    if (call.interface_name != kContractInterface) {
        if (call.interface_name.compare(0, kContractNamePrefix.size(), kContractNamePrefix) == 0) {
            return error_reply(kUnsupportedMajorError, "unsupported contract major version");
        }
        return error_reply("org.freedesktop.DBus.Error.UnknownInterface", "unknown interface");
    }
    std::size_t index = dbus_contract::kMethodNames.size();
    for (std::size_t candidate = 0; candidate < dbus_contract::kMethodNames.size(); ++candidate) {
        if (dbus_contract::kMethodNames[candidate] == call.member) {
            index = candidate;
            break;
        }
    }
    if (index == dbus_contract::kMethodNames.size()) {
        return error_reply("org.freedesktop.DBus.Error.UnknownMethod", "unknown member");
    }
    if (call.signature != dbus_contract::kMethodInSignatures[index]) {
        return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "unexpected signature");
    }
    return dispatch_member(service_, call.sender, index, call.body);
}

} // namespace aa::ipc
