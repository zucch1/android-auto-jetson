// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/trust/Identity.hpp>

#include <aa/diagnostics/Diagnostics.hpp>

#include <utility>

namespace aa::trust {

namespace {

// Identity fields come from USB/Bluetooth descriptors which may carry hostile
// bytes; only printable ASCII within the reader's string budget is accepted so
// nothing can escape the store's JSON encoding, poison a log line, or persist
// a document the loader would reject as oversized.
[[nodiscard]] bool printable_ascii(std::string_view text) {
    if (text.size() > kMaxIdentityFieldBytes) {
        return false;
    }
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte > 0x7EU) {
            return false;
        }
    }
    return true;
}

// Bluetooth addresses have one spelling for matching and revocation. Adapter
// identifiers may also be names (hci0), so only MAC-shaped adapters are folded.
[[nodiscard]] bool mac_address(std::string_view value) {
    if (value.size() != 17) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

[[nodiscard]] std::string canonical_address(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    return value;
}

// Length-prefixed concatenation is injective: distinct field tuples always
// produce distinct keys, so matching can never alias two records together.
void append_field(std::string& out, std::string_view field) {
    out += std::to_string(field.size());
    out.push_back(':');
    out.append(field);
}

// Treat blank serials as absent while retaining their bytes in identity_key.
[[nodiscard]] bool blank_serial(std::string_view serial) noexcept {
    return serial.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos;
}

[[nodiscard]] bool wired_has_material(const WiredIdentity& wired) {
    if (!blank_serial(wired.usb_serial) || !blank_serial(wired.aoa_serial)) {
        return true;
    }
    return !wired.usb_vendor_id.empty() && !wired.usb_product_id.empty() &&
           !wired.port_context.empty();
}

[[nodiscard]] bool all_fields_printable(const WiredIdentity& wired) {
    return printable_ascii(wired.usb_vendor_id) && printable_ascii(wired.usb_product_id) &&
           printable_ascii(wired.usb_serial) && printable_ascii(wired.aoa_manufacturer) &&
           printable_ascii(wired.aoa_model) && printable_ascii(wired.aoa_serial) &&
           printable_ascii(wired.port_context);
}

} // namespace

TransportKind kind_of(const TransportIdentity& identity) noexcept {
    switch (identity.index()) {
    case 0: return TransportKind::wired;
    case 1: return TransportKind::wireless;
    default: return TransportKind::opaque;
    }
}

// Classify blank serials as absent without changing the stable identity key.
IdentityStrength strength_of(const TransportIdentity& identity) noexcept {
    if (const auto* wired = std::get_if<WiredIdentity>(&identity)) {
        return blank_serial(wired->usb_serial) && blank_serial(wired->aoa_serial)
            ? IdentityStrength::port_only : IdentityStrength::serial_backed;
    }
    return kind_of(identity) == TransportKind::wireless
        ? IdentityStrength::bond_backed : IdentityStrength::opaque;
}

std::string_view strength_name(IdentityStrength strength) noexcept {
    switch (strength) {
    case IdentityStrength::serial_backed: return "serial-backed";
    case IdentityStrength::port_only: return "port-only";
    case IdentityStrength::bond_backed: return "bond-backed";
    case IdentityStrength::opaque: return "opaque";
    }
    return "opaque";
}

std::string identity_key(const TransportIdentity& identity) {
    std::string out;
    if (const auto* wired = std::get_if<WiredIdentity>(&identity)) {
        out = "w1:";
        append_field(out, wired->usb_vendor_id);
        append_field(out, wired->usb_product_id);
        append_field(out, wired->usb_serial);
        append_field(out, wired->aoa_manufacturer);
        append_field(out, wired->aoa_model);
        append_field(out, wired->aoa_serial);
        append_field(out, wired->port_context);
        return out;
    }
    if (const auto* wireless = std::get_if<WirelessIdentity>(&identity)) {
        out = "b1:";
        append_field(out, canonical_address(wireless->bond_address));
        append_field(out, mac_address(wireless->adapter) ? canonical_address(wireless->adapter) : wireless->adapter);
        return out;
    }
    const auto& opaque = std::get<OpaqueIdentity>(identity);
    return opaque.key;
}

WiredKeyClass classify_wired_key(std::string_view key) noexcept {
    constexpr std::string_view kWiredPrefix{"w1:"};
    if (key.substr(0, kWiredPrefix.size()) != kWiredPrefix) {
        return WiredKeyClass::not_wired;
    }
    // Field order is identity_key()'s: vid, pid, usb_serial, aoa_manufacturer,
    // aoa_model, aoa_serial, port_context. Anything that identity_key() could
    // not have produced (missing/extra fields, a non-canonical length with a
    // leading zero, a length past the end) is malformed and fails closed.
    constexpr std::size_t kFieldCount = 7;
    constexpr std::size_t kUsbSerial = 2;
    constexpr std::size_t kAoaSerial = 5;
    std::string_view rest = key.substr(kWiredPrefix.size());
    bool has_serial = false;
    for (std::size_t field = 0; field < kFieldCount; ++field) {
        std::size_t digits = 0;
        std::size_t length = 0;
        while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
            length = length * 10 + static_cast<std::size_t>(rest[digits] - '0');
            ++digits;
            if (length > rest.size()) {
                return WiredKeyClass::malformed;  // also bounds the accumulator
            }
        }
        if (digits == 0 || (digits > 1 && rest[0] == '0') || digits >= rest.size() ||
            rest[digits] != ':') {
            return WiredKeyClass::malformed;
        }
        rest.remove_prefix(digits + 1);
        if (length > rest.size()) {
            return WiredKeyClass::malformed;
        }
        if ((field == kUsbSerial || field == kAoaSerial) && !blank_serial(rest.substr(0, length))) {
            has_serial = true;
        }
        rest.remove_prefix(length);
    }
    if (!rest.empty()) {
        return WiredKeyClass::malformed;
    }
    return has_serial ? WiredKeyClass::serial_bound : WiredKeyClass::port_only;
}

core::Result<std::string> validated_key(const TransportIdentity& identity) {
    if (const auto* wired = std::get_if<WiredIdentity>(&identity)) {
        if (!all_fields_printable(*wired) || !wired_has_material(*wired)) {
            return Error{ErrorCode::invalid_argument};
        }
        return identity_key(identity);
    }
    if (const auto* wireless = std::get_if<WirelessIdentity>(&identity)) {
        if (!mac_address(wireless->bond_address) ||
            !printable_ascii(wireless->adapter)) {
            return Error{ErrorCode::invalid_argument};
        }
        return identity_key(identity);
    }
    const auto& opaque = std::get<OpaqueIdentity>(identity);
    if (opaque.key.empty() || !printable_ascii(opaque.key)) {
        return Error{ErrorCode::invalid_argument};
    }
    return identity_key(identity);
}

std::string redacted_label(const TransportIdentity& identity) {
    // Task-19 pseudonymization boundary: stable within a process, never the
    // raw serial, Bluetooth address or port context.
    return aa::diagnostics::redact_identifier(identity_key(identity));
}

} // namespace aa::trust
