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
    case ErrorCode::trust_not_wired_candidate: return kNotWiredCandidateError;
    case ErrorCode::invalid_argument: return "org.freedesktop.DBus.Error.InvalidArgs";
    default: return "org.freedesktop.DBus.Error.Failed";
    }
}

[[nodiscard]] OutboundReply from_void(const core::Result<void>& result) {
    return result.has_value() ? ok(std::monostate{})
                              : error_reply(error_name_for(result.error().code()),
                                            result.error().message());
}

// ConfirmPhonePairing only: a wired approval whose directory sync failed
// (#133, contract 1.1.0). Other methods keep their generic error names.
[[nodiscard]] OutboundReply from_confirm(const core::Result<void>& result) {
    if (!result.has_value() && result.error().code() == ErrorCode::trust_store_not_durable) {
        return error_reply(kNotDurableError, result.error().message());
    }
    return from_void(result);
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
        return from_confirm(
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
    case 11: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return ok(service.ping());
    }
    case 12: {
        const auto id = cursor.read_u64();
        if (!id || !body_consumed(cursor)) return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        const auto value = service.pairing_comparison(sender, core::PairingRequestId{id.value()});
        if (!value) return error_reply(error_name_for(value.error().code()), value.error().message());
        return ok(value.value());
    }
    case 13: {
        const auto request = cursor.read_u64(); const auto id = cursor.read_u64(); const auto accept = cursor.read_u32();
        if (!request || !id || !accept || accept.value() > 1 || !body_consumed(cursor))
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        return from_void(service.respond_pairing_comparison(sender, core::PairingRequestId{request.value()},
            {id.value(), accept.value() != 0}));
    }
    case 14: {
        const auto id = cursor.read_u64();
        if (!id || !body_consumed(cursor)) return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        const auto value = service.pairing_candidate(sender, core::PairingRequestId{id.value()});
        if (!value) return error_reply(error_name_for(value.error().code()), value.error().message());
        return ok(value.value());
    }
    case 15: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.request_idr_recovery(sender));
    }
    case 16: {
        if (!body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        const auto listed = service.list_phones(sender);
        if (!listed) {
            return error_reply(error_name_for(listed.error().code()), listed.error().message());
        }
        return ok(listed.value());
    }
    case 17: {
        const auto id = cursor.read_u64();
        if (!id || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        const auto outcome = service.pairing_outcome(sender, core::PairingRequestId{id.value()});
        if (!outcome) {
            return error_reply(error_name_for(outcome.error().code()), outcome.error().message());
        }
        return ok(outcome.value());
    }
    case dbus_contract::kSetProjectionModeIndex: {
        const auto mode = cursor.read_string();
        if (!mode || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.set_projection_mode(sender, mode.value()));
    }
    case dbus_contract::kSetTransportEnabledIndex: {
        const auto transport = cursor.read_string();
        const auto enabled = cursor.read_u32();
        if (!transport || !enabled || enabled.value() > 1 || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        return from_void(service.set_transport_enabled(sender, transport.value(), enabled.value() != 0));
    }
    default: return error_reply("org.freedesktop.DBus.Error.UnknownMethod", "unknown member");
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
