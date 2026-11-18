// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <gio/gunixfdlist.h>
#include <algorithm>
#include <iostream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace aa::helper {
bool BluezBackend::Impl::register_profiles() {
    struct Profile { const char* path; const char* uuid; const char* name; guint16 channel; bool& active; };
    // A26 (HFP AG) reaches WirePlumber's Hands-Free unit on scn 7 before the separate AA bootstrap on scn 8.
    const Profile profiles[]{{kProfilePath, kAaUuid, "Android Auto", 8, registered}};
    std::vector<const Profile*> added;
    const auto unregister = [&](const char* path) {
        auto* reply = call("/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile", g_variant_new("(o)", path));
        if (!reply) return false;
        g_variant_unref(reply);
        return true;
    };
    for (const auto& profile : profiles) {
        if (profile.active) continue;
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "Name", g_variant_new_string(profile.name));
        g_variant_builder_add(&options, "{sv}", "Role", g_variant_new_string("server"));
        g_variant_builder_add(&options, "{sv}", "Channel", g_variant_new_uint16(profile.channel));
        g_variant_builder_add(&options, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&options, "{sv}", "RequireAuthorization", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&options, "{sv}", "AutoConnect", g_variant_new_boolean(FALSE));
        auto* reply = call("/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile",
            g_variant_new("(os@a{sv})", profile.path, profile.uuid, g_variant_builder_end(&options)));
        if (!reply) {
            (void)unregister(profile.path);
            for (const auto* previous : added) {
                if (unregister(previous->path)) previous->active = false;
            }
            return false;
        }
        g_variant_unref(reply);
        profile.active = true;
        added.push_back(&profile);
    }
    return true;
}
bool BluezBackend::Impl::unregister_profiles() {
    if (!registered) return true;
    auto* reply = call("/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile", g_variant_new("(o)", kProfilePath));
    if (!reply) return false;
    g_variant_unref(reply);
    registered = false;
    return true;
}
namespace {
// Profile-scoped and fire-and-forget: the reply (if any) is not needed for a withdrawal.
void disconnect_hands_free(GDBusConnection* bus, const std::string& owner, const std::string& path) {
    g_dbus_connection_call(bus, owner.c_str(), path.c_str(), "org.bluez.Device1", "DisconnectProfile",
        g_variant_new("(s)", kHfpAgUuid), nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
}
}
void BluezBackend::Impl::page_replied(GObject* source, GAsyncResult* result, gpointer data) {
    auto* weak = static_cast<std::weak_ptr<PageReply>*>(data);
    GError* error = nullptr;
    auto* value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (const auto reply = weak->lock()) {
        reply->done = true;
        reply->ok = value != nullptr;
        if (error) {
            // Only the D-Bus error name: BlueZ messages are not part of the redacted journal.
            gchar* name = g_dbus_error_get_remote_error(error);
            reply->error = name ? name : "local";
            g_free(name);
        }
    }
    if (value) g_variant_unref(value);
    g_clear_error(&error);
    delete weak;
}
// A bonded phone does not page the head unit by itself: its Android Auto dials the AA RFCOMM
// profile only while it has a live link to us (it did so every ~5 s while the enrolment's HFP
// link was up, and never after that link dropped; Jetson windows 2/3, 2026-11-18).
void BluezBackend::Impl::dispatch_page() {
    if (!allowed || !bus || owner.empty()) return;
    const auto observed = *allowed;
    const auto& identity = observed.device.identity;
    // One page record at a time: a live page for this peer is kept across renewals, and a
    // retired one still awaiting its reply must resolve (and be withdrawn) first.
    if (page) return;
    const auto at = now();
    std::erase_if(paged_at, [&](const auto& entry) { return at >= entry.second + kPageInterval; });
    // Per-peer bound, independent of how fast the receiver retries after downstream errors.
    if (std::any_of(paged_at.begin(), paged_at.end(), [&](const auto& entry) { return entry.first == identity; })) return;
    // Effect-boundary recheck, after the previous window closed and the lease committed: the
    // same BlueZ owner and device/adapter incarnations, still bonded, still admitted, window live.
    Device live{};
    if (!current_owner() || !exact(observed, true, &live) || !admitted(identity) || !allowed ||
        allowed->device.path != observed.device.path || at >= window) return;
    // A live link needs no page (the phone dials AA over it) and is not ours to withdraw.
    if (live.connected) return;
    if (paged_at.size() >= 16) paged_at.erase(paged_at.begin());
    paged_at.emplace_back(identity, at);
    std::erase_if(withdrawn, [&](const Withdrawn& entry) { return entry.identity == identity; });
    page = Page{observed, false, std::make_shared<PageReply>()};
    g_main_context_push_thread_default(context);
    g_dbus_connection_call(bus, owner.c_str(), observed.device.path.c_str(), "org.bluez.Device1", "ConnectProfile",
        g_variant_new("(s)", kHfpAgUuid), nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 30000, nullptr,
        page_replied, new std::weak_ptr<PageReply>(page->reply));
    g_main_context_pop_thread_default(context);
}
void BluezBackend::Impl::service_page() {
    if (!page || !page->reply->done) return;
    const auto& reply = *page->reply;
    std::cerr << "system-helper: reconnect page " << (reply.ok ? "connected" : "failed " + reply.error)
              << (page->retired ? " after its authority ended" : "") << "\n";
    if (!reply.ok) { page.reset(); return; }
    if (page->retired) {
        // A late success after Forget, lease retirement or continuity loss gets no HFP service.
        withdraw_link(page->peer.device.path, page->peer.device.identity);
        page.reset();
    }
}
void BluezBackend::Impl::retire_page() {
    page_requested = false;
    if (!page) return;
    page->retired = true;
    if (!page->reply->done) return; // withdrawn by service_page() when the reply arrives
    const auto peer = page->peer;
    const bool linked = page->reply->ok;
    page.reset();
    if (linked) withdraw_link(peer.device.path, peer.device.identity);
}
void BluezBackend::Impl::withdraw_link(const std::string& path, const PeerIdentity& identity) {
    if (!bus || owner.empty()) return;
    disconnect_hands_free(bus, owner, path);
    std::cerr << "system-helper: reconnect page withdrawn\n";
    std::erase_if(withdrawn, [&](const Withdrawn& entry) { return entry.path == path; });
    if (withdrawn.size() >= kMaxWithdrawn) withdrawn.erase(withdrawn.begin());
    withdrawn.push_back({path, identity, std::nullopt});
}
void BluezBackend::Impl::link_changed(const std::string& path, bool up) {
    const auto at = now();
    std::erase_if(withdrawn, [&](const Withdrawn& entry) { return entry.link_down && at >= *entry.link_down + kPolicyHorizon; });
    for (auto& entry : withdrawn) {
        if (entry.path != path) continue;
        if (!up) { if (!entry.link_down) entry.link_down = at; continue; }
        // Back up with an open reconnect window for this very peer: that window's own page or
        // the phone's admitted connection, not BlueZ policy acting on withdrawn authority.
        if (allowed && allowed->device.identity == entry.identity && at < window) continue;
        if (!bus || owner.empty()) continue;
        disconnect_hands_free(bus, owner, path);
        std::cerr << "system-helper: reconnect page withdrawn again after BlueZ reconnected it\n";
    }
}
int BluezBackend::Impl::socket_fd(GDBusMethodInvocation* invocation) {
    gint index{};
    g_variant_get_child(g_dbus_method_invocation_get_parameters(invocation), 1, "h", &index);
    auto* list = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(invocation));
    if (!list || index < 0 || index >= g_unix_fd_list_get_length(list)) return -1;
    GError* error = nullptr;
    const int fd = g_unix_fd_list_get(list, index, &error);
    g_clear_error(&error);
    if (fd < 0) return -1;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISSOCK(info.st_mode)) { ::close(fd); return -1; }
    return fd;
}
}
