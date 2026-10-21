// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusService.hpp>
#include <aa/ipc/Contract.hpp>
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace aa;
using namespace aa::ipc::dbus_adapter;
ParsedMessage control(BusConnection& bus, const char* member, const char* signature = "", MessageWriter writer = {}) {
    const auto owner = bus.get_name_owner(ipc::BusName{std::string{ipc::kContractBusName}});
    if (!owner) throw std::runtime_error("receiver absent");
    writer.destination(owner.value().value).path(ipc::kContractObjectPath).interface_name(ipc::kContractInterface)
          .member(member).body_signature(signature);
    const auto sent = bus.send_message(MessageType::method_call, writer);
    if (!sent) throw std::runtime_error("send failed");
    const auto reply = bus.await_reply(sent.value(), owner.value());
    if (!reply) throw std::runtime_error("reply failed");
    return reply.value();
}
std::uint64_t request(BusConnection& bus) {
    const auto reply = control(bus, "RequestAddPhone");
    if (reply.type != MessageType::method_return || reply.signature != "t") throw std::runtime_error("request rejected");
    Cursor body{reply.body};
    return body.read_u64().value();
}
std::string command(const std::string& text) {
    std::cout << "COMMAND " << text << '\n' << std::flush;
    std::string result;
    if (!std::getline(std::cin, result)) throw std::runtime_error("fixture response missing");
    return result;
}
bool queued(const std::string& worker, bool owner_thread = false) {
    std::istringstream ids{worker};
    long pid{}, tid{};
    ids >> pid >> tid;
    if (owner_thread) tid = pid;
    const auto path = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/wchan";
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        std::string wait;
        std::ifstream{path} >> wait;
        if (wait.find("futex") != std::string::npos) return true;
        std::this_thread::yield();
    }
    return false;
}
class HelperConcurrency : public ::testing::Test {
protected:
    std::optional<BusConnection> caller;
    void SetUp() override {
        auto connection = BusConnection::connect_session(); ASSERT_TRUE(connection);
        caller.emplace(std::move(connection.value()));
        ASSERT_TRUE(caller->request_name(ipc::BusName{"org.custom.HelperConcurrentConsumer"}));
        MessageWriter name; name.put_string("org.custom.HelperConcurrentConsumer");
        ASSERT_EQ(control(*caller, "RegisterConsumer", "s", name).type, MessageType::method_return);
    }
    void TearDown() override { if (caller) (void)control(*caller, "UnregisterConsumer"); }
    std::uint64_t confirm() {
        const auto source = request(*caller);
        MessageWriter body; body.put_u64(source);
        EXPECT_EQ(control(*caller, "ConfirmPhonePairing", "t", body).type, MessageType::method_return);
        return source;
    }
};
TEST_F(HelperConcurrency, CancelledWaiterCannotInterruptOwnerOrConsumeGrant) {
    // Given an issued grant and A held inside the real backend.
    confirm();
    command("worker-release");
    ASSERT_EQ(command("wait-entered"), "entered");
    const auto worker = command("worker-acquire");
    EXPECT_TRUE(queued(worker));
    // When B is cancelled while demonstrably waiting for transaction ownership.
    EXPECT_EQ(command("stop-worker"), "0");
    EXPECT_EQ(command("join-second"), "2");
    // Then B finishes before A is released, without damaging A or its grant.
    EXPECT_EQ(command("release-held"), "released");
    EXPECT_EQ(command("join-first"), "0");
    EXPECT_EQ(command("acquire"), "0");
    EXPECT_EQ(command("status"), "0");
    EXPECT_EQ(command("reconnect"), "0");
}
TEST_F(HelperConcurrency, DeadlineExpiresWhileQueuedWithoutConsumingGrant) {
    // Given A held and B supplied a shorter total budget.
    confirm();
    command("worker-release");
    ASSERT_EQ(command("wait-entered"), "entered");
    command("worker-timeout");
    // When B completes before the backend release handshake.
    EXPECT_EQ(command("join-second"), "3");
    EXPECT_EQ(command("release-held"), "released");
    // Then A succeeds and B's unused grant still works on the same client.
    EXPECT_EQ(command("join-first"), "0");
    EXPECT_EQ(command("acquire"), "0");
    EXPECT_EQ(command("status"), "0");
    EXPECT_EQ(command("reconnect"), "0");
}
TEST_F(HelperConcurrency, ControlConfirmationAndCancelOverlapWorkers) {
    // Given real ControlService dispatch on the receiver's owning thread.
    const auto source = request(*caller);
    const auto worker = command("worker-release");
    ASSERT_EQ(command("wait-entered"), "entered");
    MessageWriter body; body.put_u64(source);
    // When confirmation queues behind a worker call on the very same client.
    auto confirmation = std::async(std::launch::async, [&] { return control(*caller, "ConfirmPhonePairing", "t", body); });
    EXPECT_TRUE(queued(worker, true));
    EXPECT_EQ(command("release-held"), "released");
    EXPECT_EQ(confirmation.get().type, MessageType::method_return);
    EXPECT_EQ(command("join-first"), "0");
    command("worker-release");
    ASSERT_EQ(command("wait-entered"), "entered");
    const auto status = command("worker-status");
    EXPECT_TRUE(queued(status));
    auto cancel = std::async(std::launch::async, [&] { return control(*caller, "CancelPhonePairing", "t", body); });
    EXPECT_TRUE(queued(worker, true));
    EXPECT_EQ(command("release-held"), "released");
    // Then both worker replies remain intact and unused cancelled grants reject.
    EXPECT_EQ(cancel.get().type, MessageType::method_return);
    EXPECT_EQ(command("join-first"), "0");
    EXPECT_EQ(command("join-second"), "0");
    EXPECT_EQ(command("acquire"), "1001");
    EXPECT_EQ(command("pair"), "1001");
    EXPECT_EQ(command("status"), "0");
    EXPECT_EQ(command("reconnect"), "0");
}
TEST_F(HelperConcurrency, DuplicateConfirmationFailureInvalidatesWithoutRecursiveLock) {
    // Given a source already confirmed directly on the store-owner thread.
    // When the helper rejects duplicate issuance, cleanup must not relock.
    EXPECT_EQ(command("duplicate-confirm"), "1001");
    // Then local grants are unusable and the same connection remains healthy.
    EXPECT_EQ(command("acquire"), "1001");
    EXPECT_EQ(command("pair"), "1001");
    EXPECT_EQ(command("status"), "0");
}
TEST(HelperBus, RegisteredConsumerTrustConfirmationAndRevocation) {
    auto caller = BusConnection::connect_session(); ASSERT_TRUE(caller);
    auto rogue = BusConnection::connect_session(); ASSERT_TRUE(rogue);
    ASSERT_TRUE(caller.value().request_name(ipc::BusName{"org.custom.HelperTestConsumer"}));
    MessageWriter name; name.put_string("org.custom.HelperTestConsumer");
    const auto registration = control(caller.value(), "RegisterConsumer", "s", name);
    ASSERT_EQ(registration.type, MessageType::method_return);
    EXPECT_EQ(command("acquire"), "1001");
    EXPECT_EQ(command("reconnect"), "701");
    const auto source = request(caller.value());
    MessageWriter confirm; confirm.put_u64(source);
    EXPECT_EQ(control(rogue.value(), "ConfirmPhonePairing", "t", confirm).type, MessageType::error_reply);
    MessageWriter forged;
    forged.path(ipc::kContractObjectPath).interface_name(ipc::kContractInterface).member("PairingOpened")
          .body_signature("t").put_u64(source);
    ASSERT_TRUE(rogue.value().send_message(MessageType::signal, forged));
    EXPECT_EQ(command("acquire"), "1001");
    EXPECT_EQ(control(caller.value(), "ConfirmPhonePairing", "t", confirm).type, MessageType::method_return);
    EXPECT_EQ(command("wrong-source"), "1001");
    EXPECT_EQ(command("cancelled"), "2");
    EXPECT_EQ(command("timeout"), "3");
    EXPECT_EQ(command("acquire"), "0");
    EXPECT_EQ(command("acquire"), "1001");
    EXPECT_EQ(command("pair"), "0");
    EXPECT_EQ(command("pair"), "1001");
    EXPECT_EQ(command("status"), "0");
    EXPECT_EQ(command("usb-status"), "0");
    EXPECT_EQ(command("modem-status"), "0");
    EXPECT_EQ(command("release"), "0");
    EXPECT_EQ(command("reconnect"), "0");
    EXPECT_EQ(command("unknown"), "701");
    EXPECT_EQ(control(caller.value(), "ConfirmPhonePairing", "t", confirm).type, MessageType::error_reply);
    MessageWriter cancel; cancel.put_u64(source);
    ASSERT_EQ(control(caller.value(), "CancelPhonePairing", "t", cancel).type, MessageType::method_return);
    auto second = request(caller.value());
    MessageWriter second_confirm; second_confirm.put_u64(second);
    ASSERT_EQ(control(caller.value(), "ConfirmPhonePairing", "t", second_confirm).type, MessageType::method_return);
    const auto phone_text = command("phone");
    ASSERT_EQ(phone_text.substr(0, 6), "phone ");
    MessageWriter phone; phone.put_u64(std::stoull(phone_text.substr(6)));
    ASSERT_EQ(control(caller.value(), "ForgetPhone", "t", phone).type, MessageType::method_return);
    EXPECT_EQ(command("acquire"), "1001");
    EXPECT_EQ(command("reconnect"), "701");
    MessageWriter close_second; close_second.put_u64(second);
    ASSERT_EQ(control(caller.value(), "CancelPhonePairing", "t", close_second).type, MessageType::method_return);
    // Confirm another source, then teardown must kill its still-pending grants.
    second = request(caller.value());
    MessageWriter third_confirm; third_confirm.put_u64(second);
    ASSERT_EQ(control(caller.value(), "ConfirmPhonePairing", "t", third_confirm).type, MessageType::method_return);
    ASSERT_EQ(control(caller.value(), "UnregisterConsumer").type, MessageType::method_return);
    EXPECT_EQ(command("acquire"), "1001");
    ASSERT_EQ(control(caller.value(), "RegisterConsumer", "s", name).type, MessageType::method_return);
    const auto last = request(caller.value());
    MessageWriter last_confirm; last_confirm.put_u64(last);
    ASSERT_EQ(control(caller.value(), "ConfirmPhonePairing", "t", last_confirm).type, MessageType::method_return);
    EXPECT_EQ(command("acquire"), "0");
}
TEST(HelperBus, WrongExecutableCannotIssueEvenWithKnownSource) {
    const char* address = std::getenv("AA_HELPER_TEST_BUS"); ASSERT_NE(address, nullptr);
    auto caller = BusConnection::connect_private(address); ASSERT_TRUE(caller);
    const auto owner = caller.value().get_name_owner(ipc::BusName{helper::kBusName}); ASSERT_TRUE(owner);
    MessageWriter writer;
    writer.destination(owner.value().value).path(helper::kObjectPath).interface_name(helper::kInterface)
          .member("Issue").body_signature("ttuu").put_u64(1).put_u64(1).put_u32(1).put_u32(36);
    const auto sent = caller.value().send_message(MessageType::method_call, writer); ASSERT_TRUE(sent);
    const auto reply = caller.value().await_reply(sent.value(), owner.value()); ASSERT_TRUE(reply);
    Cursor body{reply.value().body};
    ASSERT_EQ(reply.value().signature, "ut");
    EXPECT_EQ(body.read_u32().value(), 1001U);
}
TEST(HelperFlood, DirectedTrafficCannotStarveLiveness) {
    const char* address = std::getenv("AA_HELPER_TEST_BUS"); ASSERT_NE(address, nullptr);
    auto caller = BusConnection::connect_private(address); ASSERT_TRUE(caller);
    MessageWriter writer;
    writer.destination(helper::kBusName).path(helper::kObjectPath).interface_name(helper::kInterface)
          .member("PairingOpened").body_signature("t").put_u64(1);
    std::cout << "flood-ready\n" << std::flush;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < until) {
        caller.value().deadline(until);
        if (!caller.value().send_message(MessageType::signal, writer)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
TEST(HelperProduction, AuthorizedEffectsRemainUnavailable) {
    auto caller = BusConnection::connect_session(); ASSERT_TRUE(caller);
    ASSERT_TRUE(caller.value().request_name(ipc::BusName{"org.custom.HelperProductionConsumer"}));
    MessageWriter name; name.put_string("org.custom.HelperProductionConsumer");
    ASSERT_EQ(control(caller.value(), "RegisterConsumer", "s", name).type, MessageType::method_return);
    const auto source = request(caller.value());
    MessageWriter confirm; confirm.put_u64(source);
    ASSERT_EQ(control(caller.value(), "ConfirmPhonePairing", "t", confirm).type, MessageType::method_return);
    EXPECT_EQ(command("acquire"), "1000");
    EXPECT_EQ(command("pair"), "1000");
    EXPECT_EQ(command("acquire"), "1001");
    EXPECT_EQ(command("release"), "1000");
    EXPECT_EQ(command("reconnect"), "1000");
    EXPECT_EQ(command("status"), "0");
    EXPECT_EQ(command("usb-status"), "0");
    EXPECT_EQ(command("modem-status"), "0");
    ASSERT_EQ(control(caller.value(), "UnregisterConsumer").type, MessageType::method_return);
}
}
