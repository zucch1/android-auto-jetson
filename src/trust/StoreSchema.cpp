// SPDX-License-Identifier: GPL-3.0-or-later
#include "StoreSchema.hpp"

#include "Json.hpp"

#include <algorithm>

namespace aa::trust::detail {

namespace {

using Object = JsonValue::Object;

[[nodiscard]] Error reject() { return Error{ErrorCode::trust_store_io}; }

// Strict member lookup: the name must appear exactly once among `members`.
[[nodiscard]] const JsonValue* member(const Object& members, std::string_view name) {
    const JsonValue* found = nullptr;
    for (const auto& entry : members) {
        if (entry.first == name) {
            if (found != nullptr) {
                return found;  // duplicate already impossible: parse_json rejects
            }
            found = &entry.second;
        }
    }
    return found;
}

// Fail closed unless `members` are exactly `count` and every one is required
// (strict schema: unknown keys are a rejection, never ignored).
[[nodiscard]] core::Result<void> exact_keys(const Object& members, std::initializer_list<std::string_view> expected) {
    if (members.size() != expected.size()) {
        return reject();
    }
    for (const std::string_view name : expected) {
        if (member(members, name) == nullptr) {
            return reject();
        }
    }
    return {};
}

[[nodiscard]] core::Result<std::string> string_member(const Object& members, std::string_view name) {
    const JsonValue* value = member(members, name);
    if (value == nullptr || !value->is_string()) {
        return Error{ErrorCode::trust_store_io};
    }
    return value->as_string();
}

[[nodiscard]] core::Result<std::uint64_t> uint_member(const Object& members, std::string_view name) {
    const JsonValue* value = member(members, name);
    if (value == nullptr || !value->is_uint()) {
        return Error{ErrorCode::trust_store_io};
    }
    return value->as_uint();
}

[[nodiscard]] core::Result<TransportIdentity> parse_identity(const Object& identity) {
    const auto kind = string_member(identity, "kind");
    if (!kind.has_value()) {
        return kind.error();
    }
    if (kind.value() == "wired") {
        auto checked = exact_keys(identity, {"kind", "usb_vendor_id", "usb_product_id",
                                             "usb_serial", "aoa_manufacturer", "aoa_model",
                                             "aoa_serial", "port_context"});
        if (!checked.has_value()) {
            return checked.error();
        }
        WiredIdentity wired;
        auto field = [&identity](std::string_view name) { return string_member(identity, name); };
        for (const auto& [name, target] : {
                 std::pair<std::string_view, std::string*>{"usb_vendor_id", &wired.usb_vendor_id},
                 std::pair<std::string_view, std::string*>{"usb_product_id", &wired.usb_product_id},
                 std::pair<std::string_view, std::string*>{"usb_serial", &wired.usb_serial},
                 std::pair<std::string_view, std::string*>{"aoa_manufacturer", &wired.aoa_manufacturer},
                 std::pair<std::string_view, std::string*>{"aoa_model", &wired.aoa_model},
                 std::pair<std::string_view, std::string*>{"aoa_serial", &wired.aoa_serial},
                 std::pair<std::string_view, std::string*>{"port_context", &wired.port_context},
             }) {
            auto value = field(name);
            if (!value.has_value()) {
                return value.error();
            }
            *target = std::move(value.value());
        }
        return TransportIdentity{std::move(wired)};
    }
    if (kind.value() == "wireless") {
        auto checked = exact_keys(identity, {"kind", "bond_address", "adapter"});
        if (!checked.has_value()) {
            return checked.error();
        }
        WirelessIdentity wireless;
        auto address = string_member(identity, "bond_address");
        auto adapter = string_member(identity, "adapter");
        if (!address.has_value() || !adapter.has_value()) {
            return Error{ErrorCode::trust_store_io};
        }
        wireless.bond_address = std::move(address.value());
        wireless.adapter = std::move(adapter.value());
        return TransportIdentity{std::move(wireless)};
    }
    if (kind.value() == "opaque") {
        auto checked = exact_keys(identity, {"kind", "key"});
        if (!checked.has_value()) {
            return checked.error();
        }
        auto key = string_member(identity, "key");
        if (!key.has_value()) {
            return key.error();
        }
        return TransportIdentity{OpaqueIdentity{std::move(key.value())}};
    }
    return reject();
}

[[nodiscard]] core::Result<ApprovedPhone> parse_record(const JsonValue& value) {
    if (!value.is_object()) {
        return Error{ErrorCode::trust_store_io};
    }
    const auto& members = value.as_object();
    auto checked = exact_keys(members, {"phone_id", "identity"});
    if (!checked.has_value()) {
        return checked.error();
    }
    auto id = uint_member(members, "phone_id");
    if (!id.has_value()) {
        return id.error();
    }
    if (id.value() == 0 || id.value() > kMaxPhoneId) {
        return reject();
    }
    const JsonValue* identity = member(members, "identity");
    if (identity == nullptr || !identity->is_object()) {
        return reject();
    }
    auto parsed = parse_identity(identity->as_object());
    if (!parsed.has_value()) {
        return parsed.error();
    }
    // Every record must name its phone (identity carries usable material).
    auto key = validated_key(parsed.value());
    if (!key.has_value()) {
        return reject();
    }
    return ApprovedPhone{core::PhoneId{id.value()}, std::move(parsed.value())};
}

// Legacy version-1 record: {"phone_id": N, "key": "..."}.
[[nodiscard]] core::Result<ApprovedPhone> parse_legacy_record(const JsonValue& value) {
    if (!value.is_object()) {
        return Error{ErrorCode::trust_store_io};
    }
    const auto& members = value.as_object();
    auto checked = exact_keys(members, {"phone_id", "key"});
    if (!checked.has_value()) {
        return checked.error();
    }
    auto id = uint_member(members, "phone_id");
    auto key = string_member(members, "key");
    if (!id.has_value() || !key.has_value()) {
        return Error{ErrorCode::trust_store_io};
    }
    if (id.value() == 0 || id.value() > kMaxPhoneId) {
        return reject();
    }
    // Migration must produce a proven-safe identity: an empty or non-printable
    // legacy key is rejected exactly like any other unusable identity.
    OpaqueIdentity opaque{std::move(key.value())};
    if (!validated_key(TransportIdentity{opaque}).has_value()) {
        return reject();
    }
    return ApprovedPhone{core::PhoneId{id.value()}, TransportIdentity{std::move(opaque)}};
}

using RecordParser = core::Result<ApprovedPhone> (*)(const JsonValue&);

[[nodiscard]] core::Result<ParsedStore> parse_records(const Object& members, RecordParser parse) {
    const JsonValue* phones = member(members, "phones");
    if (phones == nullptr || !phones->is_array()) {
        return Error{ErrorCode::trust_store_io};
    }
    const auto& items = phones->as_array();
    if (items.size() > kMaxApprovedPhones) {
        return reject();
    }
    ParsedStore store;
    for (const JsonValue& item : items) {
        auto record = parse(item);
        if (!record.has_value()) {
            return record.error();
        }
        const auto duplicate = std::find_if(
            store.phones.begin(), store.phones.end(), [&record](const ApprovedPhone& existing) {
                return existing.phone_id == record.value().phone_id;
            });
        if (duplicate != store.phones.end()) {
            return reject();  // duplicate phone id
        }
        // Duplicate canonical identities are rejected in BOTH schema versions:
        // revocation is per phone id, so one identity under two ids could
        // survive a forget and silently keep reconnect eligibility.
        const std::string key = identity_key(record.value().identity);
        const auto alias = std::find_if(
            store.phones.begin(), store.phones.end(), [&key](const ApprovedPhone& existing) {
                return identity_key(existing.identity) == key;
            });
        if (alias != store.phones.end()) {
            return reject();  // duplicate canonical identity
        }
        store.phones.push_back(std::move(record.value()));
    }
    std::uint64_t highest = 0;
    for (const ApprovedPhone& phone : store.phones) {
        highest = std::max(highest, phone.phone_id.value);
    }
    store.next_phone_id = highest + 1;
    return store;
}

} // namespace

core::Result<ParsedStore> parse_store(std::string_view text) {
    if (text.empty()) {
        return ParsedStore{};
    }
    auto parsed = parse_json(text);
    if (!parsed.has_value()) {
        return Error{ErrorCode::trust_store_io};
    }
    if (!parsed.value().is_object()) {
        return reject();
    }
    const auto& members = parsed.value().as_object();
    auto version = uint_member(members, "schema_version");
    if (!version.has_value()) {
        return reject();
    }
    if (version.value() == kSchemaVersionLegacy) {
        auto checked = exact_keys(members, {"schema_version", "phones"});
        if (!checked.has_value()) {
            return checked.error();
        }
        return parse_records(members, &parse_legacy_record);
    }
    if (version.value() == kSchemaVersionCurrent) {
        auto checked = exact_keys(members, {"schema_version", "next_phone_id", "phones"});
        if (!checked.has_value()) {
            return checked.error();
        }
        auto store = parse_records(members, &parse_record);
        if (!store.has_value()) {
            return store.error();
        }
        auto next = uint_member(members, "next_phone_id");
        if (!next.has_value() || next.value() == 0 || next.value() > kMaxPhoneId + 1) {
            return reject();
        }
        // A conflicting counter (not strictly greater than every record id)
        // would reallocate a live id on the next approval: rejected, never
        // silently trusted or "fixed up".
        if (next.value() < store.value().next_phone_id) {
            return reject();
        }
        store.value().next_phone_id = next.value();
        return store;
    }
    return reject();  // unknown version: never silently trusted
}

} // namespace aa::trust::detail
