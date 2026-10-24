// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include "../../src/helper/crypto_adapter/Crypto.hpp"
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<int> secret_strings{}, secret_buffers{}, nonzero_after_wipe{};
std::atomic<bool> observing{};
constexpr std::string_view sentinel = "QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ";
}
extern "C" void real_explicit_bzero(void*, std::size_t) asm("__real_explicit_bzero");
extern "C" void tracked_explicit_bzero(void*, std::size_t) asm("__wrap_explicit_bzero");
extern "C" void tracked_explicit_bzero(void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    const bool secret = observing && std::string_view{bytes, size}.find(sentinel.substr(0, 8)) != std::string_view::npos;
    real_explicit_bzero(data, size);
    if (secret) {
        if (size > 64) ++secret_buffers; else ++secret_strings;
        for (std::size_t i = 0; i < size; ++i) if (bytes[i] != 0) { ++nonzero_after_wipe; break; }
    }
}
namespace aa::helper {
namespace {
using namespace std::chrono_literals;
class SecretLifetime : public ::testing::Test {
protected:
    void SetUp() override { secret_strings = 0; secret_buffers = 0; nonzero_after_wipe = 0; observing = true; }
    void TearDown() override { observing = false; EXPECT_EQ(nonzero_after_wipe, 0); }
};
TEST_F(SecretLifetime, CopyAssignmentWipesReplacedPasswordBeforeStorageReuse) {
    ApSnapshot target{{"ssid", sentinel, "bssid", "ip", 5277, 1}};
    const ApSnapshot source{{"ssid", "replacement", "bssid", "ip", 5277, 2}};
    target = source;
    EXPECT_GE(secret_strings, 1); EXPECT_EQ(target.password, "replacement");
}
TEST_F(SecretLifetime, MoveAssignmentWipesReplacedPasswordBeforeStorageRelease) {
    ApSnapshot target{{"ssid", sentinel, "bssid", "ip", 5277, 1}};
    ApSnapshot source{{"ssid", "replacement", "bssid", "ip", 5277, 2}};
    target = std::move(source);
    EXPECT_GE(secret_strings, 1); EXPECT_EQ(target.password, "replacement");
}
TEST_F(SecretLifetime, SmallStringMoveWipesSourceAndAssignmentWipesOldInlineSecret) {
    ApSnapshot source{{"ssid", sentinel.substr(0, 8), "bssid", "ip", 5277, 1}};
    ApSnapshot target{std::move(source)};
    EXPECT_TRUE(source.password.empty()); EXPECT_EQ(secret_strings, 1);
    target = ApSnapshot{{"ssid", "replacement", "bssid", "ip", 5277, 2}};
    EXPECT_EQ(secret_strings, 2);
}
class SecretBackend final : public Backend {
public:
    core::Result<State> apply(Action, std::uint32_t) override { return State::active; }
    void cleanup() noexcept override {}
    core::Result<ObservedPeer> observe() override { return handoff(); }
    core::Result<State> reconnect_peer(std::uint64_t) override { return State::active; }
    core::Result<ObservedPeer> handoff() override { return ObservedPeer{123, {"AA:BB:CC:DD:EE:FF", "00:11:22:33:44:55"}}; }
    core::Result<ApSnapshot> ap_snapshot() override { return ApSnapshot{{"ssid", sentinel, "02:ab:cd:ef:01:23", "10.42.0.1", 5277, 1}}; }
    bool snapshot_current(std::uint64_t epoch) override { return epoch == 1; }
    bool with_snapshot(std::uint64_t epoch, const std::function<void()>& send) override {
        if (epoch != 1) return false;
        send(); return true;
    }
};
TEST_F(SecretLifetime, CredentialRpcWipesWriterWireReceiveAndParsedBody) {
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS"); ASSERT_NE(address, nullptr);
    const int fd = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC); ASSERT_GE(fd, 0);
    const auto digest = crypto_adapter::hash_executable(fd, std::chrono::steady_clock::now() + 1s); ::close(fd);
    ASSERT_TRUE(digest);
    SecretBackend backend;
    auto service = BusService::private_bus(address, {::getuid(), digest.value()}, backend); ASSERT_TRUE(service);
    std::atomic<bool> failed{};
    std::jthread pump{[&](const std::stop_token& stop) {
        while (!stop.stop_requested()) if (!service.value().cycle()) { failed = true; return; }
    }};
    char path[] = "/tmp/task37-secret-XXXXXX"; ASSERT_NE(::mkdtemp(path), nullptr);
    trust::ApprovedPhoneStore store{std::filesystem::path{path} / "phones"}; ASSERT_TRUE(store.load());
    const trust::TransportIdentity identity{trust::WirelessIdentity{"AA:BB:CC:DD:EE:FF", "00:11:22:33:44:55"}};
    ASSERT_TRUE(store.approve(identity));
    auto client = BusClient::private_bus(address, store); ASSERT_TRUE(client);
    ASSERT_TRUE(client.value()->reconnect(identity));
    HandoffFailure failure{}; ASSERT_TRUE(client.value()->poll_bootstrap({7, 0}, failure));
    secret_strings = 0; secret_buffers = 0;
    auto snapshot = client.value()->get_ap_credentials({7, 0}, 123);
    ASSERT_TRUE(snapshot); EXPECT_TRUE(snapshot.value().password == sentinel);
    pump.request_stop(); pump.join(); EXPECT_FALSE(failed);
    EXPECT_GE(secret_buffers, 4); EXPECT_GE(secret_strings, 1);
    client.value().reset(); std::filesystem::remove_all(path);
}
class SecretReply : public SecretLifetime, public ::testing::WithParamInterface<int> {};
TEST_P(SecretReply, MalformedErrorAndSkippedTimeoutRepliesWipeAllOwnedBuffers) {
    using namespace ipc::dbus_adapter;
    auto server = BusConnection::connect_session(); ASSERT_TRUE(server);
    ASSERT_TRUE(server.value().request_name(ipc::BusName{kBusName}));
    char path[] = "/tmp/task37-secret-reply-XXXXXX"; ASSERT_NE(::mkdtemp(path), nullptr);
    trust::ApprovedPhoneStore store{std::filesystem::path{path} / "phones"}; ASSERT_TRUE(store.load());
    auto client = BusClient::private_bus(std::getenv("DBUS_SESSION_BUS_ADDRESS"), store); ASSERT_TRUE(client);
    const auto mode = GetParam();
    auto reply = std::async(std::launch::async, [&] {
        for (;;) {
            auto request = server.value().read_message();
            if (!request) return false;
            if (request.value().member != "GetAPCredentialsV1") continue;
            MessageWriter writer;
            const auto cleanup = writer.protect_body(true);
            writer.destination(request.value().sender).reply_serial(request.value().serial + (mode == 2 ? 1U : 0U))
                .body_signature("utssssut").put_u32(mode == 1 ? 1001U : 0U).put_u64(0)
                .put_string("ssid").put_string(sentinel).put_string("02:ab:cd:ef:01:23").put_string("10.42.0.1")
                .put_u32(mode == 0 ? 1U : 5277U).put_u64(1);
            return server.value().send_message(MessageType::method_return, writer, true).has_value();
        }
    });
    const auto snapshot = client.value()->get_ap_credentials({7, 0}, 123);
    EXPECT_FALSE(snapshot); EXPECT_TRUE(reply.get());
    EXPECT_GE(secret_buffers, 4);
    client.value().reset(); std::filesystem::remove_all(path);
}
INSTANTIATE_TEST_SUITE_P(CredentialCleanup, SecretReply, ::testing::Values(0, 1, 2));
TEST_F(SecretLifetime, LateCredentialReplyIsWipedWhenOrdinaryStatusDrainsItAfterTimeout) {
    using namespace ipc::dbus_adapter;
    // Given a server that cannot release credentials until the caller has timed out.
    auto server = BusConnection::connect_session(); ASSERT_TRUE(server);
    ASSERT_TRUE(server.value().request_name(ipc::BusName{kBusName}));
    char path[] = "/tmp/task37-secret-late-XXXXXX"; ASSERT_NE(::mkdtemp(path), nullptr);
    trust::ApprovedPhoneStore store{std::filesystem::path{path} / "phones"}; ASSERT_TRUE(store.load());
    auto client = BusClient::private_bus(std::getenv("DBUS_SESSION_BUS_ADDRESS"), store); ASSERT_TRUE(client);
    std::promise<void> release;
    auto allowed = release.get_future();
    auto late = std::async(std::launch::async, [&] {
        for (;;) {
            auto request = server.value().read_message();
            if (!request) return false;
            if (request.value().member != "GetAPCredentialsV1") continue;
            allowed.wait();
            {
                MessageWriter writer; const auto cleanup = writer.protect_body(true);
                writer.destination(request.value().sender).reply_serial(request.value().serial)
                    .body_signature("utssssut").put_u32(0).put_u64(0).put_string("ssid").put_string(sentinel)
                    .put_string("02:ab:cd:ef:01:23").put_string("10.42.0.1").put_u32(5277).put_u64(1);
                if (!server.value().send_message(MessageType::method_return, writer, true)) return false;
            }
            // A round trip on this same socket proves the daemon processed the late reply.
            return server.value().get_name_owner(ipc::BusName{kBusName}).has_value();
        }
    });
    const auto snapshot = client.value()->get_ap_credentials({7, 0}, 123);
    release.set_value();
    ASSERT_TRUE(late.get());
    ASSERT_FALSE(snapshot);
    EXPECT_TRUE(snapshot.error().code() == ErrorCode::timeout || snapshot.error().code() == ErrorCode::helper_unavailable);
    ASSERT_EQ(secret_buffers, 2);
    auto status_reply = std::async(std::launch::async, [&] {
        for (;;) {
            auto request = server.value().read_message();
            if (!request) return false;
            if (request.value().member != "Execute") continue;
            MessageWriter writer;
            writer.destination(request.value().sender).reply_serial(request.value().serial)
                .body_signature("ut").put_u32(0).put_u64(static_cast<std::uint64_t>(State::active));
            return server.value().send_message(MessageType::method_return, writer).has_value();
        }
    });
    // When ordinary status performs its preliminary GetNameOwner, it skips the late reply.
    const auto status = client.value()->status(Action::ap_status);
    // Then the status still succeeds, with both receiver raw and parsed copies wiped.
    EXPECT_TRUE(status_reply.get()); ASSERT_TRUE(status); EXPECT_EQ(status.value(), State::active);
    EXPECT_EQ(secret_buffers, 4);
    client.value().reset(); std::filesystem::remove_all(path);
}
TEST_F(SecretLifetime, OrdinaryReceiveOnIndependentConnectionRemainsNonSensitive) {
    using namespace ipc::dbus_adapter;
    // Given two ordinary private-bus connections with no credential transaction.
    auto server = BusConnection::connect_session(); ASSERT_TRUE(server);
    auto client = BusConnection::connect_session(); ASSERT_TRUE(client);
    MessageWriter writer;
    writer.destination(client.value().unique_name().value).reply_serial(123).body_signature("s").put_string(sentinel);
    ASSERT_TRUE(server.value().send_message(MessageType::method_return, writer));
    // When the ordinary reply is received, generic buffers are not opted into cleanup.
    const auto reply = client.value().await_reply(123, server.value().unique_name());
    // Then the data is intact and there were no secret-buffer wipes.
    ASSERT_TRUE(reply); EXPECT_EQ(reply_string(reply.value()).value(), sentinel);
    EXPECT_EQ(secret_buffers, 0);
}
TEST_F(SecretLifetime, TimedOutSendWipesWireAndWriterWithoutChangingGenericMessages) {
    using namespace ipc::dbus_adapter;
    auto bus = BusConnection::connect_session(); ASSERT_TRUE(bus);
    {
        MessageWriter writer; const auto cleanup = writer.protect_body(true);
        writer.destination("org.example.SecretSink").path("/test").interface_name("org.example.SecretSink")
            .member("Test").body_signature("s").put_string(sentinel);
        bus.value().deadline(std::chrono::steady_clock::now() - 1ms);
        EXPECT_FALSE(bus.value().send_message(MessageType::method_call, writer, true));
    }
    EXPECT_EQ(secret_buffers, 2);
    secret_buffers = 0;
    {
        MessageWriter writer;
        writer.destination("org.example.SecretSink").path("/test").interface_name("org.example.SecretSink")
            .member("Test").body_signature("s").put_string(sentinel);
        EXPECT_FALSE(bus.value().send_message(MessageType::method_call, writer));
    }
    EXPECT_EQ(secret_buffers, 0);
}
}
}
