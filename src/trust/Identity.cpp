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

// Length-prefixed concatenation is injective: distinct field tuples always
// produce distinct keys, so matching can never alias two records together.
void append_field(std::string& out, std::string_view field) {
    out += std::to_string(field.size());
    out.push_back(':');
    out.append(field);
}

[[nodiscard]] bool wired_has_material(const WiredIdentity& wired) {
    if (!wired.usb_serial.empty() || !wired.aoa_serial.empty()) {
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
        append_field(out, wireless->bond_address);
        append_field(out, wireless->adapter);
        return out;
    }
    const auto& opaque = std::get<OpaqueIdentity>(identity);
    return opaque.key;
}

core::Result<std::string> validated_key(const TransportIdentity& identity) {
    if (const auto* wired = std::get_if<WiredIdentity>(&identity)) {
        if (!all_fields_printable(*wired) || !wired_has_material(*wired)) {
            return Error{ErrorCode::invalid_argument};
        }
        return identity_key(identity);
    }
    if (const auto* wireless = std::get_if<WirelessIdentity>(&identity)) {
        if (wireless->bond_address.empty() || !printable_ascii(wireless->bond_address) ||
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
