// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/helper/bluez_adapter/Startup.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <string_view>

namespace {
enum class Overdue { none, acquisition, initialization };
thread_local Overdue overdue = Overdue::none;
thread_local gint64 clock_offset{};
thread_local unsigned initialized{}, closed{}, sockets_created{}, sockets_destroyed{};
}
extern "C" {
gint64 real_get_monotonic_time() asm("__real_g_get_monotonic_time");
GIOStream* real_address_get_stream_finish(GAsyncResult*, gchar**, GError**) asm("__real_g_dbus_address_get_stream_finish");
GDBusConnection* real_connection_new_finish(GAsyncResult*, GError**) asm("__real_g_dbus_connection_new_finish");
gboolean real_connection_close_finish(GDBusConnection*, GAsyncResult*, GError**) asm("__real_g_dbus_connection_close_finish");
GSocket* real_socket_new_from_fd(gint, GError**) asm("__real_g_socket_new_from_fd");
gint64 wrap_get_monotonic_time() asm("__wrap_g_get_monotonic_time");
GIOStream* wrap_address_get_stream_finish(GAsyncResult*, gchar**, GError**) asm("__wrap_g_dbus_address_get_stream_finish");
GDBusConnection* wrap_connection_new_finish(GAsyncResult*, GError**) asm("__wrap_g_dbus_connection_new_finish");
gboolean wrap_connection_close_finish(GDBusConnection*, GAsyncResult*, GError**) asm("__wrap_g_dbus_connection_close_finish");
GSocket* wrap_socket_new_from_fd(gint, GError**) asm("__wrap_g_socket_new_from_fd");

gint64 wrap_get_monotonic_time() {
    return real_get_monotonic_time() + clock_offset;
}
GIOStream* wrap_address_get_stream_finish(GAsyncResult* result, gchar** guid, GError** error) {
    auto* stream = real_address_get_stream_finish(result, guid, error);
    switch (overdue) {
    case Overdue::acquisition: clock_offset = G_TIME_SPAN_SECOND; break;
    case Overdue::none: case Overdue::initialization: break;
    }
    return stream;
}
GDBusConnection* wrap_connection_new_finish(GAsyncResult* result, GError** error) {
    auto* bus = real_connection_new_finish(result, error);
    ++initialized;
    // The real bus has completed Hello, but its completion beats an overdue timer.
    switch (overdue) {
    case Overdue::initialization: clock_offset = G_TIME_SPAN_SECOND; break;
    case Overdue::none: case Overdue::acquisition: break;
    }
    return bus;
}
gboolean wrap_connection_close_finish(GDBusConnection* bus, GAsyncResult* result, GError** error) {
    const auto success = real_connection_close_finish(bus, result, error);
    ++closed;
    return success;
}
GSocket* wrap_socket_new_from_fd(gint fd, GError** error) {
    auto* socket = real_socket_new_from_fd(fd, error);
    if (socket) {
        ++sockets_created;
        g_object_weak_ref(G_OBJECT(socket), [](gpointer, GObject*) { ++sockets_destroyed; }, nullptr);
    }
    return socket;
}
}
namespace {
class BluezStartup : public ::testing::Test {
protected:
    GMainContext* context{};
    std::string address;
    void SetUp() override {
        overdue = Overdue::none;
        clock_offset = 0;
        initialized = closed = sockets_created = sockets_destroyed = 0;
        const auto* value = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        ASSERT_NE(value, nullptr);
        address = value;
        context = g_main_context_new();
        g_main_context_push_thread_default(context);
    }
    void TearDown() override {
        if (!context) return;
        while (g_main_context_pending(context)) g_main_context_iteration(context, FALSE);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }
};
TEST_F(BluezStartup, BusRemainsUsableWhenIndependentDuplicateIsReleased) {
    // Given a real private message bus, not a peer-to-peer Hello substitute.
    // When startup returns a timely connection and releases its abort wrapper.
    auto* bus = aa::helper::bluez_adapter::connect_bus(address, context);
    ASSERT_NE(bus, nullptr);
    // Then the duplicate is destroyed while the assigned unique name still works.
    EXPECT_EQ(sockets_created, 1U);
    EXPECT_EQ(sockets_destroyed, 1U);
    const auto* name = g_dbus_connection_get_unique_name(bus);
    EXPECT_NE(name, nullptr);
    auto* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr);
    EXPECT_NE(reply, nullptr);
    bool found = false;
    if (reply) {
        auto* names = g_variant_get_child_value(reply, 0);
        GVariantIter iter;
        g_variant_iter_init(&iter, names);
        const char* current{};
        while (g_variant_iter_next(&iter, "&s", &current)) {
            if (name && std::string_view{current} == name) found = true;
        }
        g_variant_unref(names);
        g_variant_unref(reply);
    }
    EXPECT_TRUE(found);
    g_dbus_connection_close_sync(bus, nullptr, nullptr);
    g_object_unref(bus);
}
TEST_F(BluezStartup, LateInitializationDrainsCloseBeforeReturningUnavailable) {
    // Given actual Hello completion dispatched before an overdue timeout source.
    overdue = Overdue::initialization;
    // When the absolute deadline is checked in the initialization callback.
    auto* bus = aa::helper::bluez_adapter::connect_bus(address, context);
    // Then no bus escapes, and its explicit close callback has already drained.
    EXPECT_EQ(bus, nullptr);
    EXPECT_EQ(initialized, 1U);
    EXPECT_EQ(closed, 1U);
    EXPECT_EQ(sockets_created, 1U);
    EXPECT_EQ(sockets_destroyed, 1U);
    if (bus) { g_dbus_connection_close_sync(bus, nullptr, nullptr); g_object_unref(bus); }
}
TEST_F(BluezStartup, LateAcquisitionNeverStartsAuthentication) {
    // Given a real acquired socket whose completion beats an overdue timer.
    overdue = Overdue::acquisition;
    // When startup checks the original deadline before stream handoff.
    auto* bus = aa::helper::bluez_adapter::connect_bus(address, context);
    // Then the stream is rejected without starting another stage or deadline.
    EXPECT_EQ(bus, nullptr);
    EXPECT_EQ(initialized, 0U);
    EXPECT_EQ(sockets_created, 0U);
    if (bus) { g_dbus_connection_close_sync(bus, nullptr, nullptr); g_object_unref(bus); }
}
}
