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
            (void)g_variant_lookup(props, "Bonded", "b", &bonded);
            if (g_variant_lookup(props, "Address", "&s", &address) && address_valid(address) &&
                g_variant_lookup(props, "AddressType", "&s", &type) && std::string_view{type} == "public" &&
                g_variant_lookup(props, "Adapter", "&o", &adapter) &&
                g_variant_lookup(props, "Paired", "b", &paired) && devices.size() < 256) {
                devices.push_back({path, adapter, {address, {}}, paired != FALSE && bonded != FALSE});
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
bool BluezBackend::Impl::exact(const Observation& observed, bool require_bonded) {
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
    return std::any_of(devices.begin(), devices.end(), [&](const Device& device) {
        return device.path == observed.device.path && device.adapter_path == observed.device.adapter_path &&
               device.identity == observed.device.identity && (!require_bonded || device.bonded);
    });
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
void BluezBackend::Impl::remember(const PeerIdentity& identity) {
    if (std::find(managed.begin(), managed.end(), identity) == managed.end()) managed.push_back(identity);
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
    if (!observed || impl_->allowed || impl_->managed.size() >= 256) return Error{ErrorCode::helper_rejected};
    impl_->allowed = *observed;
    impl_->pairing_enabled = true;
    impl_->comparison_used = false;
    impl_->remember(observed->device.identity);
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
    if (!observed || !impl_->exact(*observed, true) || impl_->managed.size() >= 256) return Error{ErrorCode::helper_rejected};
    impl_->close_window();
    impl_->allowed = *observed;
    impl_->window = impl_->now() + std::chrono::seconds(60);
    impl_->remember(observed->device.identity);
    // The AA profile is a server: the bonded phone initiates RFCOMM. No generic
    // Device1.Connect (which could activate unrelated audio profiles) is used.
    return State::active;
}
core::Result<void> BluezBackend::revoke_peer(const PeerIdentity& identity) {
    if (!address_valid(identity.address) || !address_valid(identity.adapter) ||
        std::find(impl_->managed.begin(), impl_->managed.end(), identity) == impl_->managed.end()) return Error{ErrorCode::helper_rejected};
    if (!impl_->current_owner()) return Error{ErrorCode::helper_unavailable};
    if ((impl_->allowed && impl_->allowed->device.identity == identity) ||
        (impl_->connected && impl_->connected->device.identity == identity) ||
        (impl_->hfp_peer && impl_->hfp_peer->device.identity == identity)) impl_->close_window();
    std::erase_if(impl_->observations, [&](const auto& item) { return item.second.device.identity == identity; });
    std::vector<Impl::Device> devices;
    if (!impl_->inventory(devices)) return Error{ErrorCode::helper_unavailable};
    for (const auto& device : devices) {
        if (device.identity != identity) continue;
        auto* reply = impl_->call(device.adapter_path.c_str(), "org.bluez.Adapter1", "RemoveDevice", g_variant_new("(o)", device.path.c_str()));
        if (!reply) return Error{ErrorCode::helper_unavailable};
        g_variant_unref(reply);
        return {};
    }
    return {};
}
}
