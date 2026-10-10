// SPDX-License-Identifier: GPL-3.0-or-later
#include "StoreSchema.hpp"

#include "Json.hpp"

namespace aa::trust::detail {

namespace {

void write_identity(std::string& out, const TransportIdentity& identity) {
    out += "{\"kind\":\"";
    if (const auto* wired = std::get_if<WiredIdentity>(&identity)) {
        out += "wired\",";
        out += "\"usb_vendor_id\":";
        append_json_string(out, wired->usb_vendor_id);
        out += ",\"usb_product_id\":";
        append_json_string(out, wired->usb_product_id);
        out += ",\"usb_serial\":";
        append_json_string(out, wired->usb_serial);
        out += ",\"aoa_manufacturer\":";
        append_json_string(out, wired->aoa_manufacturer);
        out += ",\"aoa_model\":";
        append_json_string(out, wired->aoa_model);
        out += ",\"aoa_serial\":";
        append_json_string(out, wired->aoa_serial);
        out += ",\"port_context\":";
        append_json_string(out, wired->port_context);
        out += "}";
        return;
    }
    if (const auto* wireless = std::get_if<WirelessIdentity>(&identity)) {
        out += "wireless\",\"bond_address\":";
        append_json_string(out, wireless->bond_address);
        out += ",\"adapter\":";
        append_json_string(out, wireless->adapter);
        out += "}";
        return;
    }
    out += "opaque\",\"key\":";
    append_json_string(out, std::get<OpaqueIdentity>(identity).key);
    out += "}";
}

} // namespace

std::string write_store(const ParsedStore& store) {
    std::string out;
    out += "{\"schema_version\":";
    out += std::to_string(kSchemaVersionCurrent);
    out += ",\"next_phone_id\":";
    out += std::to_string(store.next_phone_id);
    out += ",\"phones\":[";
    bool first = true;
    for (const ApprovedPhone& phone : store.phones) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "{\"phone_id\":";
        out += std::to_string(phone.phone_id.value);
        out += ",\"identity\":";
        write_identity(out, phone.identity);
        out += "}";
    }
    out += "]}";
    return out;
}

} // namespace aa::trust::detail
