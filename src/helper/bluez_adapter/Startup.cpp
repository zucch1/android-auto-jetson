// SPDX-License-Identifier: GPL-3.0-or-later
#include "Startup.hpp"
#include <fcntl.h>
#include <unistd.h>

namespace aa::helper::bluez_adapter {
namespace {
struct Startup {
    const gint64 deadline = g_get_monotonic_time() + 300 * G_TIME_SPAN_MILLISECOND;
    GCancellable* cancel = g_cancellable_new();
    GIOStream* stream{};
    GSocket* abort_socket{};
    GDBusConnection* bus{};
    bool expired{};
    bool done{};

    void abort() {
        if (expired) return;
        expired = true;
        g_cancellable_cancel(cancel);
        if (abort_socket) {
            // This independent wrapper owns a dup of the kernel endpoint, never
            // GDBus's stream/socket object. Closing just the dup cannot wake Hello.
            GError* error = nullptr;
            (void)g_socket_shutdown(abort_socket, TRUE, TRUE, &error);
            g_clear_error(&error);
        }
    }
    bool timely() {
        // Completions can dispatch before an already-overdue timeout source.
        if (g_get_monotonic_time() >= deadline) abort();
        return !expired;
    }
    static void closed(GObject* source, GAsyncResult* result, gpointer data) {
        auto& state = *static_cast<Startup*>(data);
        (void)g_dbus_connection_close_finish(G_DBUS_CONNECTION(source), result, nullptr);
        (void)state.timely();
        g_clear_object(&state.bus);
        state.done = true;
    }
    static void initialized(GObject*, GAsyncResult* result, gpointer data) {
        auto& state = *static_cast<Startup*>(data);
        state.bus = g_dbus_connection_new_finish(result, nullptr);
        const bool timely = state.timely();
        if (state.bus) {
            g_dbus_connection_set_exit_on_close(state.bus, FALSE);
            if (!timely) {
                // Never publish a late result; retain state through close_finish.
                g_dbus_connection_close(state.bus, nullptr, closed, &state);
                return;
            }
        }
        state.done = true;
    }
    static void acquired(GObject*, GAsyncResult* result, gpointer data) {
        auto& state = *static_cast<Startup*>(data);
        state.stream = g_dbus_address_get_stream_finish(result, nullptr, nullptr);
        const bool timely = state.timely();
        if (!timely || !state.stream || !G_IS_SOCKET_CONNECTION(state.stream)) {
            state.done = true;
            return;
        }
        auto* socket = g_socket_connection_get_socket(G_SOCKET_CONNECTION(state.stream));
        const int duplicate = ::fcntl(g_socket_get_fd(socket), F_DUPFD_CLOEXEC, 0);
        if (duplicate < 0) {
            state.done = true;
            return;
        }
        state.abort_socket = g_socket_new_from_fd(duplicate, nullptr);
        if (!state.abort_socket) {
            ::close(duplicate); // Ownership transfers only on successful wrapping.
            state.done = true;
            return;
        }
        if (!state.timely()) {
            state.done = true;
            return;
        }
        // After handoff, stream is retained only for unref after callbacks drain.
        g_dbus_connection_new(state.stream, nullptr,
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                             G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, state.cancel, initialized, &state);
    }
};
}
GDBusConnection* connect_bus(const std::string& address, GMainContext* context) {
    Startup pending;
    auto* timeout = g_timeout_source_new(300);
    g_source_set_ready_time(timeout, pending.deadline);
    g_source_set_callback(timeout, [](gpointer data) -> gboolean {
        static_cast<Startup*>(data)->abort();
        return G_SOURCE_REMOVE;
    }, &pending, nullptr);
    g_source_attach(timeout, context);
    g_dbus_address_get_stream(address.c_str(), pending.cancel, Startup::acquired, &pending);
    // Each stage either completes or hands off to the next, including late close.
    while (!pending.done) g_main_context_iteration(context, TRUE);
    g_source_destroy(timeout);
    g_source_unref(timeout);
    g_clear_object(&pending.cancel);
    g_clear_object(&pending.stream);
    // Timely success must close only our duplicate, not shut down the live bus.
    g_clear_object(&pending.abort_socket);
    return pending.bus;
}
}
