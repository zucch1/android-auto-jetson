// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <climits>
#include <optional>
#include <string_view>

namespace aa::ipc {

// Versioned identity of the session D-Bus control contract (task 21). The
// introspection XML (docs/dbus/org.custom.AndroidAutoReceiver1.xml) is the
// contract authority; tools/ipc/dbus_contract.py validates it against the
// checked schema and embeds it for the runtime introspection below. The
// interface name encodes the contract MAJOR version: clients bind
// org.custom.AndroidAutoReceiver1 and any other major (e.g.
// org.custom.AndroidAutoReceiver2) is rejected by is_supported_contract().
//
// SemVer metadata lives on the interface (ContractSemVer property and the
// matching annotation); its major always equals the interface-name major.
inline constexpr std::string_view kContractInterface = "org.custom.AndroidAutoReceiver1";
inline constexpr std::string_view kContractObjectPath = "/org/custom/AndroidAutoReceiver";
inline constexpr unsigned kContractMajor = 1;

// Parses the trailing MAJOR version out of a candidate interface name
// ("org.custom.AndroidAutoReceiver3" -> 3). Names outside the
// org.custom.AndroidAutoReceiverN pattern (including dotted suffixes and
// leading-zero majors) yield no value; a major that overflows unsigned
// arithmetic fails closed as no value.
[[nodiscard]] constexpr std::optional<unsigned> contract_major(
    std::string_view interface_name) noexcept {
    constexpr std::string_view prefix = "org.custom.AndroidAutoReceiver";
    if (interface_name.size() <= prefix.size() ||
        interface_name.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    const auto digits = interface_name.substr(prefix.size());
    unsigned major = 0;
    if (digits.size() > 1 && digits.front() == '0') {
        return std::nullopt;
    }
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        const unsigned value = static_cast<unsigned>(digit - '0');
        if (major > (UINT_MAX - value) / 10U) {
            return std::nullopt;
        }
        major = major * 10U + value;
    }
    return major;
}

// True only for the exact supported contract name. Unknown and out-of-range
// majors (e.g. org.custom.AndroidAutoReceiver2, and digit strings whose
// decimal value overflows unsigned arithmetic) fail closed.
[[nodiscard]] constexpr bool is_supported_contract(std::string_view interface_name) noexcept {
    return interface_name == kContractInterface;
}

// Runtime introspection of the checked schema (exact checked-in XML bytes) and
// its SemVer contract metadata, generated from the contract authority at build
// time. Served verbatim for D-Bus Introspect; compared against the checked
// schema file by the introspection-equality tests.
[[nodiscard]] std::string_view introspection_xml() noexcept;
[[nodiscard]] std::string_view contract_semver() noexcept;

} // namespace aa::ipc
