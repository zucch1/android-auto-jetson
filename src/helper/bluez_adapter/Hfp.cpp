// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <gio/gunixfdlist.h>
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
