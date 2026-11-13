// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/trust/Store.hpp>

#include "StoreFile.hpp"
#include "StoreSchema.hpp"

#include <algorithm>
#include <utility>

namespace aa::trust {

namespace {

[[nodiscard]] bool usable_key(const PhoneIdentity& identity) {
    return !identity.key.empty();
}

} // namespace

core::Result<std::filesystem::path> ApprovedPhoneStore::default_path() {
    return detail::resolve_default_path();
}

core::Result<void> ApprovedPhoneStore::load() {
    dropped_port_only_count_ = 0;
    auto text = detail::read_file(path_);
    if (!text.has_value()) {
        usable_ = false;
        phones_.clear();
        return text.error();
    }
    if (!text.value().empty()) {
        auto checked = detail::ensure_private_file(path_);
        if (!checked.has_value()) {
            usable_ = false;
            phones_.clear();
            return checked.error();
        }
    }
    auto parsed = detail::parse_store(text.value());
    if (!parsed.has_value()) {
        usable_ = false;
        phones_.clear();
        return parsed.error();
    }
    dropped_port_only_count_ = parsed.value().dropped_port_only_count;
    phones_ = std::move(parsed.value().phones);
    next_phone_id_ = parsed.value().next_phone_id;
    loaded_ = true;
    usable_ = true;
    return {};
}

core::Result<void> ApprovedPhoneStore::persist() {
    if (!usable_) {
        return Error{ErrorCode::trust_store_io};
    }
    detail::ParsedStore snapshot{phones_, next_phone_id_};
    const std::string text = detail::write_store(snapshot);
    // Serialized-document budget enforced BEFORE the write commits: a
    // document the reader would reject is never persisted (callers roll the
    // in-memory mutation back on this error).
    if (text.size() > detail::kMaxStoreBytes) {
        return Error{ErrorCode::trust_store_io};
    }
    return detail::write_file_private(path_, text);
}

std::optional<std::size_t> ApprovedPhoneStore::index_of_key(const std::string& key) const {
    for (std::size_t index = 0; index < phones_.size(); ++index) {
        if (identity_key(phones_[index].identity) == key) {
            return index;
        }
    }
    return std::nullopt;
}

Decision ApprovedPhoneStore::lookup(const PhoneIdentity& identity) const {
    if (!usable_ || !usable_key(identity)) {
        return Decision::unknown;  // fail closed
    }
    return index_of_key(identity.key).has_value() ? Decision::approved : Decision::unknown;
}

core::Result<void> ApprovedPhoneStore::approve_once(const PhoneIdentity& identity) {
    if (!usable_) { return Error{ErrorCode::trust_unknown_phone}; }
    auto approved = approve(TransportIdentity{OpaqueIdentity{identity.key}});
    if (!approved.has_value()) {
        return approved.error();
    }
    return {};
}

core::Result<core::PhoneId> ApprovedPhoneStore::approve(const TransportIdentity& identity) {
    return approve_impl(identity, false);
}

core::Result<core::PhoneId> ApprovedPhoneStore::approve_port_only(const WiredIdentity& identity) {
    if (strength_of(TransportIdentity{identity}) != IdentityStrength::port_only) {
        return Error{ErrorCode::invalid_argument};
    }
    return approve_impl(TransportIdentity{identity}, true);
}

core::Result<core::PhoneId> ApprovedPhoneStore::approve_impl(
    const TransportIdentity& identity, bool allow_port_only) {
    if (!usable_) {
        return Error{ErrorCode::trust_store_io};
    }
    auto key = validated_key(identity);
    if (!key.has_value()) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    if (strength_of(identity) == IdentityStrength::port_only && !allow_port_only) {
        return Error{ErrorCode::trust_port_only_requires_opt_in};
    }
    // The opaque compatibility seam cannot bypass structured wired approval.
    if (const auto* opaque = std::get_if<OpaqueIdentity>(&identity);
        opaque != nullptr && opaque->key.starts_with("w1:")) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    if (const auto existing = index_of_key(key.value()); existing.has_value()) {
        return phones_[existing.value()].phone_id;  // approve-once
    }
    if (phones_.size() >= kMaxApprovedPhones || next_phone_id_ > kMaxPhoneId) {
        return Error{ErrorCode::trust_store_io};  // id range exhausted: fail closed
    }
    const core::PhoneId phone{next_phone_id_};
    phones_.push_back(ApprovedPhone{phone, identity, strength_of(identity)});
    ++next_phone_id_;
    auto saved = persist();
    if (!saved.has_value()) {
        // 703 keeps the committed-but-not-durable mutation (memory == disk);
        // pre-replacement errors roll the allocation back.
        if (saved.error().code() != ErrorCode::trust_store_not_durable) {
            phones_.pop_back();
            --next_phone_id_;
        }
        return saved.error();
    }
    return phone;
}

core::Result<void> ApprovedPhoneStore::forget(const PhoneIdentity& identity) {
    if (!usable_ || !usable_key(identity)) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    const auto index = index_of_key(identity.key);
    if (!index.has_value()) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    return forget_phone(phones_[index.value()].phone_id);
}

core::Result<void> ApprovedPhoneStore::forget_phone(core::PhoneId phone) {
    if (!usable_) {
        return Error{ErrorCode::trust_store_io};
    }
    if (!phone) {
        return Error{ErrorCode::invalid_argument};
    }
    const auto index = std::find_if(phones_.begin(), phones_.end(), [phone](const ApprovedPhone& entry) {
        return entry.phone_id == phone;
    });
    if (index == phones_.end()) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    // Identity-safe revocation: forget removes EVERY record carrying the same
    // canonical identity, so the identity cannot remain approved under a
    // second phone id (defense in depth; the loader already rejects stores
    // containing such duplicates).
    const std::string key = identity_key(index->identity);
    const std::vector<ApprovedPhone> before = phones_;
    phones_.erase(std::remove_if(phones_.begin(), phones_.end(),
                                 [&key](const ApprovedPhone& entry) {
                                     return identity_key(entry.identity) == key;
                                 }),
                  phones_.end());
    auto saved = persist();
    if (!saved.has_value()) {
        // 703: the revocation renamed and landed on disk — KEEP it in memory
        // too (no restore of `before`); pre-replacement errors roll back so a
        // forget only lands when persisted.
        if (saved.error().code() != ErrorCode::trust_store_not_durable) {
            phones_ = before;
        }
        return saved.error();
    }
    return {};
}

std::string ApprovedPhoneStore::redacted_summary() const {
    std::string out;
    for (const ApprovedPhone& phone : phones_) {
        if (!out.empty()) {
            out += " ";
        }
        out += "phone-";
        out += std::to_string(phone.phone_id.value);
        out += "=";
        out += redacted_label(phone.identity);
        out += "[";
        out += strength_name(phone.strength);
        out += "]";
    }
    return out;
}

} // namespace aa::trust
