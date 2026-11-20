// SPDX-License-Identifier: GPL-3.0-or-later
// #89 L12: the bus socket itself is authenticated before any RPC. A listener
// whose kernel-reported owner (SO_PEERCRED) the caller does not admit never
// receives a single byte, and a server whose SASL GUID differs from the
// address's guid= pin never receives BEGIN, so neither Hello nor any helper
// call reaches a spoofed endpoint.
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>

#include <gtest/gtest.h>

namespace {

using aa::ErrorCode;
namespace bus = aa::ipc::dbus_adapter;

// A Unix listener owned by this test's UID; records every byte a client sends.
class FakeBus final {
public:
    FakeBus() {
        char directory[] = "/tmp/aa-l12-XXXXXX";
        if (::mkdtemp(directory) == nullptr) return;
        directory_ = directory;
        path_ = directory_ + "/bus";
        listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1);
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 1) != 0) {
            ::close(listener_);
            listener_ = -1;
        }
    }
    FakeBus(const FakeBus&) = delete;
    FakeBus& operator=(const FakeBus&) = delete;
    ~FakeBus() {
        if (listener_ >= 0) ::close(listener_);
        if (!directory_.empty()) std::filesystem::remove_all(directory_);
    }
    [[nodiscard]] bool ready() const { return listener_ >= 0; }
    [[nodiscard]] std::string address(const std::string& extra = {}) const {
        return "unix:path=" + path_ + extra;
    }
    // Accepts one client and returns everything it sent until EOF or 2 s of
    // silence. With `ok_line`, answers the first CRLF-terminated line with it.
    [[nodiscard]] std::future<std::string> serve(std::string ok_line = {}) {
        return std::async(std::launch::async, [this, ok_line] {
            std::string received;
            pollfd ready{listener_, POLLIN, 0};
            if (::poll(&ready, 1, 2000) != 1) return received;
            const int peer = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            if (peer < 0) return received;
            bool answered = ok_line.empty();
            for (;;) {
                pollfd readable{peer, POLLIN, 0};
                if (::poll(&readable, 1, 2000) != 1) break;
                char chunk[256];
                const auto got = ::read(peer, chunk, sizeof(chunk));
                if (got <= 0) break;
                received.append(chunk, static_cast<std::size_t>(got));
                if (!answered && received.find("\r\n") != std::string::npos) {
                    answered = true;
                    if (::write(peer, ok_line.data(), ok_line.size()) < 0) break;
                }
            }
            ::close(peer);
            return received;
        });
    }

private:
    std::string directory_;
    std::string path_;
    int listener_{-1};
};

constexpr const char* kGuidA = "0123456789abcdef0123456789abcdef";
constexpr const char* kGuidB = "fedcba9876543210fedcba9876543210";

TEST(BusSocketAuth, ForeignOwnedSocketIsRefusedBeforeAnyByte) {
    FakeBus fake;
    ASSERT_TRUE(fake.ready());
    auto received = fake.serve();
    // The listener belongs to this UID; demand some other, non-root owner.
    const bus::BusOwner other{static_cast<std::uint32_t>(::getuid()) + 1, false};
    const auto connected = bus::BusConnection::connect_private(fake.address(), other);
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code(), ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(received.get(), "");
}

TEST(BusSocketAuth, SystemBusPolicyRefusesANonRootSocket) {
    if (::getuid() == 0) GTEST_SKIP() << "a root-owned listener is the legitimate system bus";
    FakeBus fake;
    ASSERT_TRUE(fake.ready());
    auto received = fake.serve();
    // A spoofed /run/dbus/system_bus_socket created by an unprivileged user.
    const auto connected = bus::BusConnection::connect_private(fake.address(), bus::BusOwner::root());
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code(), ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(received.get(), "");
}

TEST(BusSocketAuth, OwnerPolicyAdmitsOnlyTheNamedUidOrRoot) {
    EXPECT_TRUE(bus::BusOwner::root().admits(0));
    EXPECT_FALSE(bus::BusOwner::root().admits(1000));
    const auto self = static_cast<std::uint32_t>(::getuid());
    EXPECT_TRUE(bus::BusOwner::root_or_self().admits(self));
    EXPECT_TRUE(bus::BusOwner::root_or_self().admits(0));
    EXPECT_FALSE(bus::BusOwner::root_or_self().admits(self + 1));
}

TEST(BusSocketAuth, PinnedGuidMismatchNeverSendsBegin) {
    FakeBus fake;
    ASSERT_TRUE(fake.ready());
    auto received = fake.serve(std::string{"OK "} + kGuidA + "\r\n");
    const auto connected =
        bus::BusConnection::connect_private(fake.address(std::string{",guid="} + kGuidB));
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code(), ErrorCode::transport_unauthorized_peer);
    const auto bytes = received.get();
    EXPECT_NE(bytes.find("AUTH EXTERNAL"), std::string::npos);
    EXPECT_EQ(bytes.find("BEGIN"), std::string::npos);
}

TEST(BusSocketAuth, MalformedServerGuidNeverSendsBegin) {
    FakeBus fake;
    ASSERT_TRUE(fake.ready());
    auto received = fake.serve("OK not-a-guid\r\n");
    const auto connected = bus::BusConnection::connect_private(fake.address());
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code(), ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(received.get().find("BEGIN"), std::string::npos);
}

TEST(BusSocketAuth, MalformedOrRepeatedGuidPinIsRefused) {
    FakeBus fake;
    ASSERT_TRUE(fake.ready());
    EXPECT_FALSE(bus::BusConnection::connect_private(fake.address(",guid=xyz")));
    EXPECT_FALSE(bus::BusConnection::connect_private(
        fake.address(std::string{",guid="} + kGuidA + ",guid=" + kGuidA)));
}

// The private host-verification bus: a dbus-daemon this UID started.
TEST(BusSocketAuth, OwnPrivateBusWithItsGuidIsAdmitted) {
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(address, nullptr);
    ASSERT_NE(std::string{address}.find("guid="), std::string::npos);
    EXPECT_TRUE(bus::BusConnection::connect_private(address));
    EXPECT_TRUE(bus::BusConnection::connect_private(address, bus::BusOwner::root_or_self()));
}

TEST(BusSocketAuth, OwnPrivateBusWithAnotherGuidIsRefused) {
    const std::string address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const auto at = address.find("guid=");
    ASSERT_NE(at, std::string::npos);
    std::string forged = address;
    forged.replace(at + 5, 32, forged.compare(at + 5, 32, kGuidA) == 0 ? kGuidB : kGuidA);
    const auto connected = bus::BusConnection::connect_private(forged);
    ASSERT_FALSE(connected);
    EXPECT_EQ(connected.error().code(), ErrorCode::transport_unauthorized_peer);
}

} // namespace
