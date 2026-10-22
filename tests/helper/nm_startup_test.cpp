// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/helper/nm/Platform.hpp"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto manager = "org.freedesktop.NetworkManager";
class NmStartup : public ::testing::Test {
protected:
    std::string directory, address;
    int listener{-1}, peer{-1}, pidfd{-1};
    pid_t child{};
    Clock::time_point started;
    void SetUp() override {
        directory = std::string{AA_HELPER_BUILD} + "/nm-startup-XXXXXX";
        ASSERT_NE(::mkdtemp(directory.data()), nullptr);
        const auto path = directory + "/bus";
        address = "unix:path=" + path;
        listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_GE(listener, 0);
        sockaddr_un socket_address{};
        socket_address.sun_family = AF_UNIX;
        ASSERT_LT(path.size(), sizeof(socket_address.sun_path));
        std::copy(path.begin(), path.end(), socket_address.sun_path);
        ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&socket_address), sizeof(socket_address)), 0);
        ASSERT_EQ(::listen(listener, 1), 0);
    }
    void launch(bool healthy = false) {
        char executable[] = "/proc/self/exe", probe[] = "--probe";
        char yes[] = "healthy", no[] = "unavailable";
        char* args[]{executable, probe, address.data(), healthy ? yes : no, nullptr};
        started = Clock::now();
        ASSERT_EQ(::posix_spawn(&child, executable, nullptr, nullptr, args, environ), 0);
        pidfd = static_cast<int>(::syscall(SYS_pidfd_open, child, 0));
        ASSERT_GE(pidfd, 0);
    }
    void accept_peer() {
        ::pollfd event{listener, POLLIN, 0};
        ASSERT_EQ(::poll(&event, 1, 1000), 1);
        peer = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        ASSERT_GE(peer, 0);
    }
    std::string line() {
        std::string result;
        while (result.size() < 256) {
            ::pollfd event{peer, POLLIN, 0};
            if (::poll(&event, 1, 1000) != 1) return result;
            char byte{};
            if (::recv(peer, &byte, 1, 0) != 1) return result;
            result += byte;
            if (byte == '\n') return result;
        }
        return result;
    }
    void send(const std::string& text) {
        ASSERT_EQ(::send(peer, text.data(), text.size(), MSG_NOSIGNAL), static_cast<ssize_t>(text.size()));
    }
    void authenticate() {
        ASSERT_EQ(line().substr(0, 5), std::string("\0AUTH", 5));
        send("REJECTED EXTERNAL\r\n");
        ASSERT_EQ(line().substr(0, 14), "AUTH EXTERNAL ");
        send("OK 0123456789abcdef0123456789abcdef\r\n");
        ASSERT_EQ(line(), "NEGOTIATE_UNIX_FD\r\n");
        send("AGREE_UNIX_FD\r\n");
        ASSERT_EQ(line(), "BEGIN\r\n");
        std::array<guchar, 4096> bytes{};
        std::size_t used{};
        gssize needed{};
        do {
            ::pollfd event{peer, POLLIN, 0};
            ASSERT_EQ(::poll(&event, 1, 1000), 1);
            const auto count = ::recv(peer, bytes.data() + used, bytes.size() - used, 0);
            ASSERT_GT(count, 0);
            used += static_cast<std::size_t>(count);
            needed = g_dbus_message_bytes_needed(bytes.data(), used, nullptr);
            ASSERT_GE(needed, 0);
            ASSERT_LT(used, bytes.size());
        } while (needed == 0 || used < static_cast<std::size_t>(needed));
        auto* hello = g_dbus_message_new_from_blob(bytes.data(), used, G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, nullptr);
        ASSERT_NE(hello, nullptr);
        EXPECT_STREQ(g_dbus_message_get_member(hello), "Hello");
        EXPECT_STREQ(g_dbus_message_get_destination(hello), "org.freedesktop.DBus");
        g_object_unref(hello);
        std::cout << "Captured AUTH EXTERNAL, UNIX_FD negotiation, BEGIN and binary Hello (" << used << " bytes)\n";
    }
    void bounded_exit() {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(2s - (Clock::now() - started));
        ASSERT_GT(remaining.count(), 0);
        ::pollfd event{pidfd, POLLIN, 0};
        ASSERT_EQ(::poll(&event, 1, static_cast<int>(remaining.count())), 1)
            << "real NM Connection constructor/destructor exceeded 2 seconds; guard will kill and reap child";
        int status{};
        ASSERT_EQ(::waitpid(child, &status, 0), child);
        child = 0;
        EXPECT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);
        EXPECT_LT(Clock::now() - started, 2s);
    }
    void TearDown() override {
        if (child) {
            EXPECT_EQ(::kill(child, SIGKILL), 0);
            EXPECT_EQ(::waitpid(child, nullptr, 0), child);
            std::cout << "Guard killed and reaped constructor child " << child << '\n';
        }
        for (const auto fd : {pidfd, peer, listener}) if (fd >= 0) ::close(fd);
        std::filesystem::remove_all(directory);
    }
};
TEST_F(NmStartup, FailsWithinTwoSecondsWhenAuthStalls) {
    // Given a private endpoint that accepts but never replies to AUTH.
    launch(); accept_peer();
    ASSERT_EQ(line().substr(0, 5), std::string("\0AUTH", 5));
    // When the real Connection constructor reaches its startup deadline.
    // Then it fails closed, drains and exits before the external guard.
    bounded_exit();
}
TEST_F(NmStartup, FailsWithinTwoSecondsWhenHelloStalls) {
    // Given successful real GIO authentication and a captured binary Hello.
    launch(); accept_peer(); authenticate();
    // When the endpoint withholds Hello's reply.
    // Then cancellation must interrupt the uncancellable Hello transport read.
    bounded_exit();
}
TEST_F(NmStartup, RemainsHealthyWhenPrivateBusCompletesHello) {
    // Given a real private dbus-daemon with a pinned NM service owner.
    auto* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    address = g_test_dbus_get_bus_address(bus);
    auto* owner = g_dbus_connection_new_for_address_sync(address.c_str(),
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                         G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
    ASSERT_NE(owner, nullptr);
    g_dbus_connection_set_exit_on_close(owner, FALSE);
    auto* reply = g_dbus_connection_call_sync(owner, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", manager, 4U), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 300, nullptr, nullptr);
    ASSERT_NE(reply, nullptr);
    g_variant_unref(reply);
    // When the actual constructor uses full GIO message-bus initialization.
    launch(true);
    // Then the duplicate's release must leave the live connection healthy.
    bounded_exit();
    (void)g_dbus_connection_close_sync(owner, nullptr, nullptr);
    g_object_unref(owner);
    g_test_dbus_down(bus);
    g_object_unref(bus);
}
}
int main(int argc, char** argv) {
    if (argc == 4 && std::string{argv[1]} == "--probe") {
        const auto started = Clock::now();
        bool healthy{};
        {
            aa::helper::nm::Connection connection{argv[2]};
            healthy = connection.healthy();
        }
        const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        std::cout << "real Connection healthy=" << healthy << " elapsed=" << elapsed << "s\n";
        return healthy == (std::string{argv[3]} == "healthy") && elapsed < 2.0 ? 0 : 1;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
