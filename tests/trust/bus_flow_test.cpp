// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 pairing flow across a real private bus: RequestAddPhone and
// ConfirmPhonePairing are dispatched with the daemon-stamped sender through
// the task-21 call boundary, and the trust store only changes when the same
// one-time request id is confirmed. Unauthorized callers never modify it.
#include <aa/ipc/BusPeerLookup.hpp>
#include <aa/ipc/ControlDispatcher.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <aa/trust/Store.hpp>

#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include "../dbus/fakes.hpp"
#include "fakes.hpp"

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
using aa::ErrorCode;
using aa::ipc::BusName;
using aa::ipc::BusPeerLookup;
using aa::ipc::ControlDispatcher;
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;
namespace bus = aa::ipc::dbus_adapter;

constexpr const char* kReceiverName = "org.custom.AndroidAutoReceiver";
constexpr const char* kReceiverPath = "/org/custom/AndroidAutoReceiver";
constexpr const char* kConsumerName = "org.custom.AndroidAutoReceiverTest.Consumer";

struct TrustBus final {
    test::TempStoreDir dir{};
    trust::ApprovedPhoneStore store{dir.file()};
    test::ManualClock clock{};
    trust::PairingPolicy policy{store, clock};
    std::optional<bus::BusConnection> server{};
    std::optional<bus::BusConnection> client{};
    BusPeerLookup lookup{};
    aa::ipc::test::RecordingEventSink events{};
    aa::ipc::test::SequencePairingIds ids{};
    std::unique_ptr<ControlService> service{};
    std::unique_ptr<ControlDispatcher> dispatcher{};

    void start() {
        static_cast<void>(store.load());
        auto server_conn = bus::BusConnection::connect_session();
        auto client_conn = bus::BusConnection::connect_session();
        ASSERT_TRUE(server_conn.has_value());
        ASSERT_TRUE(client_conn.has_value());
        server = std::move(server_conn.value());
        client = std::move(client_conn.value());
        ASSERT_TRUE(server->request_name(BusName{kReceiverName}).has_value());
        ASSERT_TRUE(client->request_name(BusName{kConsumerName}).has_value());
        ASSERT_TRUE(lookup.connected());
        service = std::make_unique<ControlService>(
            ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
            ControlServiceDeps{lookup, events, ids, policy});
        dispatcher = std::make_unique<ControlDispatcher>(*service);
    }
};

[[nodiscard]] aa::core::Result<bus::ParsedMessage> round_trip(
    TrustBus& bus_state, std::string_view member, const bus::MessageWriter& body_writer) {
    bus::MessageWriter call = body_writer;
    call.destination(kReceiverName).path(kReceiverPath).interface_name(
        "org.custom.AndroidAutoReceiver1").member(member);
    const auto serial = bus_state.client->send_message(bus::MessageType::method_call, call);
    if (!serial.has_value()) {
        return serial.error();
    }
    auto incoming = bus_state.server->read_message();
    if (!incoming.has_value()) {
        return incoming.error();
    }
    const auto call_view = bus::to_incoming_call(incoming.value());
    const auto reply = bus_state.dispatcher->handle(call_view);
    if (!reply.has_value()) {
        return aa::Error{ErrorCode::ipc_malformed_request};
    }
    const auto sent =
        bus_state.server->send_reply(incoming.value().serial, call_view.sender, *reply);
    if (!sent.has_value()) {
        return sent.error();
    }
    return bus_state.client->await_reply(serial.value(), bus_state.server->unique_name());
}

[[nodiscard]] std::uint64_t parse_u64(const bus::ParsedMessage& message) {
    bus::Cursor cursor(message.body);
    const auto value = cursor.read_u64();
    return value.has_value() ? value.value() : 0;
}

void register_consumer(TrustBus& bus_state) {
    bus::MessageWriter register_body;
    register_body.body_signature("s").put_string(kConsumerName);
    const auto registered = round_trip(bus_state, "RegisterConsumer", register_body);
    ASSERT_TRUE(registered.has_value());
    ASSERT_EQ(registered.value().type, bus::MessageType::method_return);
}

TEST(TrustBusFlow, ApproveOnceOverTheWireThenForget) {
    TrustBus bus_state;
    bus_state.start();
    register_consumer(bus_state);

    bus_state.policy.note_candidate(trust::TransportIdentity{test::kWired});
    bus::MessageWriter empty;
    empty.body_signature("");
    const auto requested = round_trip(bus_state, "RequestAddPhone", empty);
    ASSERT_TRUE(requested.has_value());
    ASSERT_EQ(requested.value().type, bus::MessageType::method_return);
    const std::uint64_t request_id = parse_u64(requested.value());

    ASSERT_TRUE(bus_state.store.phones().empty());

    bus::MessageWriter confirm;
    confirm.body_signature("t").put_u64(request_id);
    const auto confirmed = round_trip(bus_state, "ConfirmPhonePairing", confirm);
    ASSERT_TRUE(confirmed.has_value());
    ASSERT_EQ(confirmed.value().type, bus::MessageType::method_return);

    const auto identity = test::identity_of(trust::TransportIdentity{test::kWired});
    EXPECT_EQ(bus_state.store.lookup(identity), trust::Decision::approved);
    ASSERT_EQ(bus_state.store.phones().size(), 1U);

    bus::MessageWriter forget;
    forget.body_signature("t").put_u64(bus_state.store.phones()[0].phone_id.value);
    const auto forgotten = round_trip(bus_state, "ForgetPhone", forget);
    ASSERT_TRUE(forgotten.has_value());
    ASSERT_EQ(forgotten.value().type, bus::MessageType::method_return);
    EXPECT_EQ(bus_state.store.lookup(identity), trust::Decision::unknown);
}

TEST(TrustBusFlow, ExpiredConfirmOverTheWireIsRejectedNotPersisted) {
    TrustBus bus_state;
    bus_state.start();
    register_consumer(bus_state);

    bus_state.policy.note_candidate(trust::TransportIdentity{test::kWired});
    bus::MessageWriter empty;
    empty.body_signature("");
    const auto requested = round_trip(bus_state, "RequestAddPhone", empty);
    ASSERT_TRUE(requested.has_value());

    bus_state.clock.advance(
        aa::core::Milliseconds{trust::PairingPolicy::kConfirmWindow.count + 1});
    bus::MessageWriter confirm;
    confirm.body_signature("t").put_u64(parse_u64(requested.value()));
    const auto expired = round_trip(bus_state, "ConfirmPhonePairing", confirm);

    ASSERT_TRUE(expired.has_value());
    ASSERT_EQ(expired.value().type, bus::MessageType::error_reply);
    EXPECT_EQ(expired.value().error_name, "org.freedesktop.DBus.Error.Failed");
    bus::Cursor body(expired.value().body);
    const auto message = body.read_string();
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message.value(), aa::Error{ErrorCode::session_pairing_timeout}.message());
    EXPECT_TRUE(bus_state.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(bus_state.dir.file()));
}

TEST(TrustBusFlow, UnregisteredAddPhoneOverTheWireLeavesStoreUnmodified) {
    TrustBus bus_state;
    bus_state.start();

    bus_state.policy.note_candidate(trust::TransportIdentity{test::kWired});
    bus::MessageWriter empty;
    empty.body_signature("");
    const auto denied = round_trip(bus_state, "RequestAddPhone", empty);

    ASSERT_TRUE(denied.has_value());
    ASSERT_EQ(denied.value().type, bus::MessageType::error_reply);
    EXPECT_EQ(denied.value().error_name,
              "org.custom.AndroidAutoReceiver1.Error.ConsumerRejected");
    EXPECT_TRUE(bus_state.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(bus_state.dir.file()));
}

} // namespace
