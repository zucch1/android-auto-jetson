// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/trust/Store.hpp>

#include "StoreFile.hpp"
#include "StoreSchema.hpp"

#include <algorithm>
#include <cstdio>
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
    ++reload_epoch_;
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
    if (parsed.value().invalid_wireless_count || parsed.value().colliding_wireless_count) {
        auto saved = detail::backup_file_private(path_, text.value());
        if (saved) saved = detail::write_file_private(path_, detail::write_store(parsed.value()));
        if (!saved) {
            usable_ = false;
            phones_.clear();
            return saved.error();
        }
        std::fprintf(stderr, "trust store: quarantined %zu invalid legacy wireless address records; "
            "%zu address-case collision records; explicit re-pairing required\n",
            parsed.value().invalid_wireless_count, parsed.value().colliding_wireless_count);
    }
    dropped_port_only_count_ = parsed.value().dropped_port_only_count;
    phones_ = std::move(parsed.value().phones);
    ++mutation_epoch_;
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
    // Ordered with the off-owner revocation writes: this document is the newest.
    const std::uint64_t generation = sequencer_->next.fetch_add(1);
    std::unique_lock lock(sequencer_->mutex, std::defer_lock);
    if (!lock.try_lock_for(kOwnerWriteWait)) {
        return Error{ErrorCode::trust_store_io}; // a stuck writer: fail closed, nothing replaced
    }
    if (generation <= sequencer_->committed) {
        return Error{ErrorCode::trust_store_io};
    }
    auto written = detail::write_file_private(path_, text);
    if (written.has_value() || written.error().code() == ErrorCode::trust_store_not_durable) {
        sequencer_->committed = generation;
    }
    return written;
}

std::optional<std::size_t> ApprovedPhoneStore::index_of_key(const std::string& key) const {
    // Port-independent for serial-bound wired keys, exact otherwise (#133).
    const std::string wanted = match_key(key);
    for (std::size_t index = 0; index < phones_.size(); ++index) {
        if (match_key(phones_[index].identity) == wanted) {
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
    const auto* wired = std::get_if<WiredIdentity>(&identity);
    auto key = wired ? validated_wired_phone_key(*wired) : validated_key(identity);
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
    ++mutation_epoch_;
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
    auto prepared = prepare_forget(phone);
    if (!prepared.has_value()) {
        return prepared.error();
    }
    return apply_forget(prepared.value(), write_prepared(prepared.value()));
}

core::Result<PreparedForget> ApprovedPhoneStore::prepare_forget(core::PhoneId phone) const {
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
    // matching identity (match_key: a serial-backed wired phone recorded on
    // two ports is ONE phone), so the identity cannot remain approved under a
    // second phone id or a second port record.
    PreparedForget prepared{phone, match_key(index->identity), {}, path_, mutation_epoch_,
                            sequencer_->next.fetch_add(1), sequencer_};
    std::vector<ApprovedPhone> after;
    for (const auto& entry : phones_) {
        if (match_key(entry.identity) != prepared.match_key) {
            after.push_back(entry);
        }
    }
    prepared.document = detail::write_store(detail::ParsedStore{std::move(after), next_phone_id_});
    // Serialized-document budget enforced BEFORE the write commits.
    if (prepared.document.size() > detail::kMaxStoreBytes) {
        return Error{ErrorCode::trust_store_io};
    }
    return prepared;
}

core::Result<void> ApprovedPhoneStore::write_prepared(const PreparedForget& prepared) {
    if (!prepared.sequencer) {
        return Error{ErrorCode::invalid_argument};
    }
    auto& sequencer = *prepared.sequencer;
    const std::lock_guard lock(sequencer.mutex);
    if (prepared.generation <= sequencer.committed) {
        // A newer document already replaced the file: never restore this stale one.
        return Error{ErrorCode::cancelled};
    }
    auto written = detail::write_file_private(prepared.path, prepared.document);
    if (written.has_value() || written.error().code() == ErrorCode::trust_store_not_durable) {
        sequencer.committed = prepared.generation;
    }
    return written;
}

core::Result<void> ApprovedPhoneStore::apply_forget(const PreparedForget& prepared,
                                                   core::Result<void> written) {
    // 703: the revocation renamed and landed on disk, so it is kept in memory
    // too; a pre-replacement error leaves memory (and trust) unchanged.
    if (superseded(prepared)) {
        // Another mutation landed after this document was built, so it is
        // stale (it either never replaced the file, or a newer write already
        // overwrote it). Change nothing; the caller prepares again.
        return Error{ErrorCode::cancelled};
    }
    if (!written.has_value() && written.error().code() != ErrorCode::trust_store_not_durable) {
        return written;
    }
    phones_.erase(std::remove_if(phones_.begin(), phones_.end(),
                                 [&prepared](const ApprovedPhone& entry) {
                                     return match_key(entry.identity) == prepared.match_key;
                                 }),
                  phones_.end());
    ++mutation_epoch_;
    return written;
}

core::Result<void> ApprovedPhoneStore::sync_directory() const {
    if (!usable_) {
        return Error{ErrorCode::trust_store_io};
    }
    return detail::sync_parent_directory(path_);
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

core::Result<std::vector<std::map<std::string, std::string>>> list_approved_phones(
    const ApprovedPhoneStore& store) {
    if (!store.usable()) {
        return Error{ErrorCode::trust_store_io};
    }
    std::vector<std::map<std::string, std::string>> listed;
    listed.reserve(store.phones().size());
    for (const ApprovedPhone& phone : store.phones()) {
        std::string transport = "opaque";
        switch (kind_of(phone.identity)) {
        case TransportKind::wired:
            transport = "wired";
            break;
        case TransportKind::wireless:
            transport = "wireless";
            break;
        case TransportKind::opaque: {
            const auto& key = std::get<OpaqueIdentity>(phone.identity).key;
            if (classify_wired_key(key) == WiredKeyClass::serial_bound || key.starts_with("w1:")) {
                transport = "wired";
            } else if (key.starts_with("b1:")) {
                transport = "wireless";
            }
            break;
        }
        }
        listed.push_back(std::map<std::string, std::string>{
            {"phone_id", std::to_string(phone.phone_id.value)},
            {"label", redacted_label(phone.identity)},
            {"transport", std::move(transport)},
            {"strength", std::string{strength_name(phone.strength)}},
        });
    }
    return listed;
}

} // namespace aa::trust
