// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include "../crypto_adapter/Crypto.hpp"
#include <algorithm>

namespace aa::helper {
namespace {
bool address_valid(const std::string& address) {
    if (address.size() != 17) return false;
    for (std::size_t i = 0; i < address.size(); ++i) {
        const char c = address[i];
        if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
}
bool BluezBackend::Impl::inventory(std::vector<Device>& devices) {
    auto* reply = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
                       G_VARIANT_TYPE("(a{oa{sa{sv}}})"));
    if (!reply) return false;
    if (g_variant_get_size(reply) > std::size_t{1024} * 1024) { g_variant_unref(reply); return false; }
    auto* objects = g_variant_get_child_value(reply, 0);
    std::map<std::string, std::string> adapter_ids;
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const char* path{};
    GVariant* interfaces{};
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        auto* props = g_variant_lookup_value(interfaces, "org.bluez.Adapter1", G_VARIANT_TYPE_VARDICT);
        if (props) {
            const char* address{};
            if (g_variant_lookup(props, "Address", "&s", &address) && address_valid(address)) adapter_ids.emplace(path, address);
            g_variant_unref(props);
        }
        props = g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE_VARDICT);
        if (props) {
            const char *address{}, *adapter{}, *type{};
            gboolean paired = FALSE;
            gboolean bonded = FALSE;
            gboolean connected = FALSE;
            (void)g_variant_lookup(props, "Bonded", "b", &bonded);
            (void)g_variant_lookup(props, "Connected", "b", &connected);
            if (g_variant_lookup(props, "Address", "&s", &address) && address_valid(address) &&
                g_variant_lookup(props, "AddressType", "&s", &type) && std::string_view{type} == "public" &&
                g_variant_lookup(props, "Adapter", "&o", &adapter) &&
                g_variant_lookup(props, "Paired", "b", &paired) && devices.size() < 256) {
                devices.push_back({path, adapter, {address, {}}, paired != FALSE && bonded != FALSE, connected != FALSE});
            }
            g_variant_unref(props);
        }
        g_variant_unref(interfaces);
    }
    if (adapter_ids.size() != 1) {
        g_variant_unref(objects); g_variant_unref(reply); return false;
    }
    adapters.clear();
    for (const auto& [adapter, id] : adapter_ids) { (void)id; adapters.push_back(adapter); }
    for (auto& device : devices) {
        const auto adapter = adapter_ids.find(device.adapter_path);
        if (adapter != adapter_ids.end()) device.identity.adapter = adapter->second;
    }
    std::erase_if(devices, [](const Device& device) { return device.identity.adapter.empty(); });
    g_variant_unref(objects); g_variant_unref(reply);
    return true;
}
bool BluezBackend::Impl::exact(const Observation& observed, bool require_bonded, Device* live) {
    drain_events();
    const auto current = [&] {
        return observed.owner == owner && observed.owner_epoch == owner_epoch &&
            observed.device_epoch == incarnations[observed.device.path] &&
            observed.adapter_epoch == incarnations[observed.device.adapter_path];
    };
    if (!current_owner() || !current()) return false;
    std::vector<Device> devices;
    if (!inventory(devices)) return false;
    drain_events();
    if (!current_owner() || !current()) return false;
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const Device& device) {
        return device.path == observed.device.path && device.adapter_path == observed.device.adapter_path &&
               device.identity == observed.device.identity && (!require_bonded || device.bonded);
    });
    if (found == devices.end()) return false;
    if (live) *live = *found;
    return true;
}
std::optional<BluezBackend::Impl::Observation> BluezBackend::Impl::consume(std::uint64_t handle) {
    if (!ensure()) return std::nullopt;
    const auto found = observations.find(handle);
    if (found == observations.end()) return std::nullopt;
    auto observed = found->second;
    observations.erase(found);
    if (now() >= observed.expires || !exact(observed, false)) return std::nullopt;
    return observed;
}
core::Result<ObservedPeer> BluezBackend::observe() {
    if (!impl_->ensure()) return Error{ErrorCode::helper_unavailable};
    std::vector<Impl::Device> devices;
    if (!impl_->inventory(devices)) return Error{ErrorCode::helper_unavailable};
    if (devices.size() != 1 || impl_->observations.size() >= 16) return Error{ErrorCode::helper_rejected};
    const auto id = crypto_adapter::random_id();
    if (!id || !id.value() || impl_->observations.contains(id.value())) return Error{ErrorCode::helper_rejected};
    impl_->observations.emplace(id.value(), Impl::Observation{devices.front(), impl_->owner,
        impl_->now() + std::chrono::seconds(60), impl_->owner_epoch,
        impl_->incarnations[devices.front().path], impl_->incarnations[devices.front().adapter_path]});
    return ObservedPeer{id.value(), devices.front().identity};
}
core::Result<State> BluezBackend::authorize(std::uint64_t handle) {
    const auto observed = impl_->consume(handle);
    if (!observed || impl_->allowed) return Error{ErrorCode::helper_rejected};
    // A currently managed identity is enrolled already (it reconnects); it must be forgotten
    // (demoted to revoked) before a new enrolment, whose release would otherwise apply to it.
    // No enrolment opens before this helper run's first reconciliation: until the receiver has
    // answered, the journal may hold an approved bond whose commit never landed.
    if (!impl_->reconciled) return Error{ErrorCode::helper_unavailable};
    if (!impl_->pending.valid() || !impl_->revoked.valid() || impl_->pending.peers().size() >= 256 ||
        impl_->pending.contains(observed->device.identity) || impl_->managed.contains(observed->device.identity))
        return Error{ErrorCode::helper_rejected};
    // Durably note the enrolment before any window opens: until its commit it is never admitted,
    // and after a restart the receiver's reconciliation decides (an approval whose commit never
    // landed is admitted; anything else is released, its bond kept). A failed write refuses it.
    if (!impl_->pending.remember(observed->device.identity)) return Error{ErrorCode::helper_unavailable};
    // Revocation history stays: only this enrolment's commit (or reconciliation of its approval
    // after an acknowledgement) clears it, so a window that closes without acceptance cannot turn a
    // resurrected or stale approval into an admission.
    std::erase(impl_->awaiting, observed->device.identity);
    impl_->allowed = *observed;
    impl_->pairing_enabled = true;
    impl_->comparison_used = false;
    impl_->comparison_accepted = false;
    impl_->acknowledged = false;
    impl_->window = impl_->now() + std::chrono::seconds(60);
    if (!impl_->set(observed->device.adapter_path, "PairableTimeout", g_variant_new_uint32(60)) ||
        !impl_->set(observed->device.adapter_path, "DiscoverableTimeout", g_variant_new_uint32(60)) ||
        !impl_->set(observed->device.adapter_path, "Pairable", g_variant_new_boolean(TRUE)) ||
        !impl_->set(observed->device.adapter_path, "Discoverable", g_variant_new_boolean(TRUE)) ||
        !impl_->current_owner() || !impl_->allowed) {
        impl_->close_window(); return Error{ErrorCode::helper_unavailable};
    }
    return State::active;
}
core::Result<State> BluezBackend::reconnect_peer(std::uint64_t handle) {
    const auto observed = impl_->consume(handle);
    // Only a bond this helper itself durably admitted may reconnect: a reconnect
    // never creates revocation authority for a bond the helper did not create.
    // Already-admitted bonds need no write or fsync, so they keep working when
    // persistence later fails; if durability is still unconfirmed, retry the
    // directory fsync once before refusing.
    if (!observed || !impl_->exact(*observed, true) || !impl_->admitted(observed->device.identity))
        return Error{ErrorCode::helper_rejected};
    // A page owned for another identity ends with this window's authority.
    if (impl_->page && impl_->page->peer.device.identity != observed->device.identity) impl_->retire_page();
    impl_->close_window();
    impl_->allowed = *observed;
    impl_->window = impl_->now() + std::chrono::seconds(60);
    // The AA profile is a server: the bonded phone initiates RFCOMM, but only over a live link.
    // page_reconnected() pages its HFP Audio Gateway once the caller has committed the lease.
    // No generic Device1.Connect (which could activate unrelated audio profiles) is used.
    impl_->page_requested = true;
    return State::active;
}
void BluezBackend::page_reconnected() {
    if (!impl_->page_requested) return;
    impl_->page_requested = false;
    impl_->dispatch_page();
}
core::Result<void> BluezBackend::revoke_peer(const PeerIdentity& identity) {
    // Authority: an admitted bond, a revoked (historical) one, or an open enrolment's journal entry
    // (unaccepted or provisional; the receiver retires it after a failed write or commit). Without
    // it nothing is touched. The Bluetooth adapter is shared (docs/enrolment-ordering.md, #136): a
    // revocation never removes or disconnects a bond. A managed bond is demoted to history and its
    // journal entry dropped; from then on it is refused for admission, the Android Auto profile, HFP
    // authorization and reconnect, and only the owner (or infotainment) unpairs it.
    if (!address_valid(identity.address) || !address_valid(identity.adapter)) return Error{ErrorCode::helper_rejected};
    const bool authority = impl_->managed.revocable(identity) || impl_->revoked.contains(identity) ||
                           impl_->pending.contains(identity);
    if (!impl_->current_owner()) return Error{ErrorCode::helper_unavailable};
    if (!authority) return Error{ErrorCode::helper_rejected};
    // The receiver revokes it: no longer awaiting its answer.
    if ((impl_->allowed && impl_->allowed->device.identity == identity) ||
        (impl_->connected && impl_->connected->device.identity == identity)) impl_->close_window();
    std::erase(impl_->awaiting, identity);
    // A page for this identity loses its authority now; a link it created is withdrawn.
    if (impl_->page && impl_->page->peer.device.identity == identity) impl_->retire_page();
    // Demoted first: once forgotten, the bond is never admitted (services, reconnect) again and is
    // never taken as acceptance of a later enrolment of the same identity. Success only once every
    // write is confirmed durable; otherwise the receiver retries.
    const bool demoted = impl_->demote(identity);
    std::erase_if(impl_->observations, [&](const auto& item) { return item.second.device.identity == identity; });
    if (!demoted || (impl_->pending.contains(identity) && !impl_->pending.forget(identity)))
        return Error{ErrorCode::helper_unavailable};
    return {};
}
}
