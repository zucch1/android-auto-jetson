// SPDX-License-Identifier: GPL-3.0-or-later
#include "PropertiesDispatch.hpp"

#include <aa/ipc/Contract.hpp>

#include "BusWire.hpp"
#include "DispatchReply.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace aa::ipc {

namespace {

constexpr std::string_view kContractPropertyName = "ContractSemVer";

// Interface targeting for a Properties call: the empty interface resolves to
// the sole property-bearing interface, contract-name majors (including
// decimal-overflow digit strings) fail as unsupported majors, and everything
// else is an unknown interface.
[[nodiscard]] std::optional<OutboundReply> property_interface_error(std::string_view name) {
    if (name.empty() || is_supported_contract(name)) {
        return std::nullopt;
    }
    if (name.compare(0, kContractNamePrefix.size(), kContractNamePrefix) == 0) {
        return error_reply(kUnsupportedMajorError, "unsupported contract major version");
    }
    return error_reply("org.freedesktop.DBus.Error.UnknownInterface", "unknown interface");
}

} // namespace

// Public org.freedesktop.DBus.Properties access to the contract metadata (no
// registration required, like GetCapabilities/introspection): Get returns
// ContractSemVer as a string variant, GetAll the a{sv} dictionary, and every
// well-formed Set attempt is rejected read-only. Exact in-signatures, complete
// strings and variants and full body consumption are validated before any
// target is interpreted; the variant must contain exactly one "s".
std::optional<OutboundReply> dispatch_properties(const IncomingCall& call) {
    if (call.member == "Get") {
        if (call.signature != "ss") {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "unexpected signature");
        }
        dbus_adapter::Cursor cursor(call.body);
        auto interface_name = cursor.read_string();
        auto property = cursor.read_string();
        if (!interface_name.has_value() || !property.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        if (const auto target = property_interface_error(interface_name.value())) {
            return target.value();
        }
        if (property.value() != kContractPropertyName) {
            return error_reply("org.freedesktop.DBus.Error.UnknownProperty", "unknown property");
        }
        return ok(StringPropertyValue{std::string{contract_semver()}});
    }
    if (call.member == "GetAll") {
        if (call.signature != "s") {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "unexpected signature");
        }
        dbus_adapter::Cursor cursor(call.body);
        auto interface_name = cursor.read_string();
        if (!interface_name.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        if (const auto target = property_interface_error(interface_name.value())) {
            return target.value();
        }
        PropertyDictionary dictionary;
        dictionary.entries.emplace(std::string{kContractPropertyName},
                                   std::string{contract_semver()});
        return ok(std::move(dictionary));
    }
    if (call.member == "Set") {
        if (call.signature != "ssv") {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "unexpected signature");
        }
        dbus_adapter::Cursor cursor(call.body);
        auto interface_name = cursor.read_string();
        auto property = cursor.read_string();
        auto contained = cursor.read_signature();
        if (!interface_name.has_value() || !property.has_value() || !contained.has_value()) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        if (contained.value() != "s") {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs",
                               "unexpected variant type");
        }
        auto value = cursor.read_string();
        if (!value.has_value() || !body_consumed(cursor)) {
            return error_reply("org.freedesktop.DBus.Error.InvalidArgs", "malformed arguments");
        }
        if (const auto target = property_interface_error(interface_name.value())) {
            return target.value();
        }
        if (property.value() != kContractPropertyName) {
            return error_reply("org.freedesktop.DBus.Error.UnknownProperty", "unknown property");
        }
        return error_reply("org.freedesktop.DBus.Error.PropertyReadOnly", "read-only property");
    }
    return error_reply("org.freedesktop.DBus.Error.UnknownMethod", "unknown member");
}

} // namespace aa::ipc
