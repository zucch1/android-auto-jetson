// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 call-dispatch boundary on a private bus (gate round 1, finding 5):
// real client method calls cross the actual bus, the daemon stamps the
// sender, and ControlDispatcher routes to ControlService authorization with
// that identity. RegisterConsumer/StartProjection/StopProjection, the pairing
// flow and introspection all run through the boundary; the caller never
// supplies its own identity.
#include <aa/ipc/BusPeerLookup.hpp>
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/ControlDispatcher.hpp>
#include <aa/ipc/ControlService.hpp>

#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include "fakes.hpp"

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace {

using aa::ErrorCode;
using aa::ipc::BusName;
using aa::ipc::BusPeerLookup;
using aa::ipc::ControlDispatcher;
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;
using aa::ipc::ProjectionState;
namespace bus = aa::ipc::dbus_adapter;

constexpr const char* kReceiverName = "org.custom.AndroidAutoReceiver";
constexpr const char* kReceiverPath = "/org/custom/AndroidAutoReceiver";
constexpr const char* kConsumerName = "org.custom.AndroidAutoReceiverTest.Consumer";

struct BoundaryBus final {
    std::optional<bus::BusConnection> server{};
    std::optional<bus::BusConnection> client{};
    BusPeerLookup lookup{};
    aa::ipc::test::RecordingEventSink events{};
    aa::ipc::test::SequencePairingIds ids{};
    aa::ipc::test::RecordingPhoneDirectory phones{};
    std::unique_ptr<ControlService> service{};
    std::unique_ptr<ControlDispatcher> dispatcher{};

    void start() {
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
            ControlServiceDeps{lookup, events, ids, phones});
        dispatcher = std::make_unique<ControlDispatcher>(*service);
    }
};

[[nodiscard]] aa::core::Result<bus::ParsedMessage> round_trip(BoundaryBus& bus_state,
                                                              std::string_view interface_name,
                                                              std::string_view member,
                                                              const bus::MessageWriter& body_writer) {
    bus::MessageWriter call = body_writer;
    call.destination(kReceiverName).path(kReceiverPath).interface_name(interface_name).member(member);
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
        return aa::Error{aa::ErrorCode::ipc_malformed_request};
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

[[nodiscard]] std::string parse_string(const bus::ParsedMessage& message) {
    bus::Cursor cursor(message.body);
    auto value = cursor.read_string();
    return value.has_value() ? value.value() : std::string{};
}

TEST(BusBoundary, RealCallsDispatchThroughBoundaryWithDaemonStampedIdentity) {
    // Given: a receiver name owned by the server and a consumer name owned by
    // the client, with production authorization on the real peer credentials.
    BoundaryBus bus_state;
    bus_state.start();

    // When: the full control flow crosses the bus as real method calls.
    bus::MessageWriter register_body;
    register_body.body_signature("s").put_string(kConsumerName);
    const auto registered =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "RegisterConsumer", register_body);
    ASSERT_TRUE(registered.has_value()) << "registration must pass real uid/ownership checks";
    ASSERT_EQ(registered.value().type, bus::MessageType::method_return);
    ASSERT_EQ(registered.value().signature, "t");
    const auto consumer_id = parse_u64(registered.value());
    EXPECT_NE(consumer_id, 0U);

    bus::MessageWriter empty;
    empty.body_signature("");
    ASSERT_TRUE(round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "StartProjection", empty)
                    .has_value());
    const auto state =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "GetState", empty);
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state.value().signature, "st");
    bus::Cursor state_cursor(state.value().body);
    const auto projection = state_cursor.read_string();
    const auto state_id = state_cursor.read_u64();
    ASSERT_TRUE(projection.has_value());
    EXPECT_EQ(projection.value(), "projecting");
    ASSERT_TRUE(state_id.has_value());
    EXPECT_EQ(state_id.value(), consumer_id);

    const auto request =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "RequestAddPhone", empty);
    ASSERT_TRUE(request.has_value());
    ASSERT_EQ(request.value().signature, "t");
    const auto request_id = parse_u64(request.value());
    EXPECT_NE(request_id, 0U);
    EXPECT_FALSE(bus_state.service->pairing_window_open());

    bus::MessageWriter confirm_body;
    confirm_body.body_signature("t").put_u64(request_id);
    ASSERT_TRUE(
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "ConfirmPhonePairing", confirm_body)
            .has_value());

    ASSERT_TRUE(round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "StopProjection", empty)
                    .has_value());

    bus::MessageWriter introspect_body;
    introspect_body.body_signature("");
    const auto introspect =
        round_trip(bus_state, "org.freedesktop.DBus.Introspectable", "Introspect", introspect_body);
    ASSERT_TRUE(introspect.has_value());
    ASSERT_EQ(introspect.value().signature, "s");

    // Then: the pairing window only opened via the confirmed request and the
    // runtime introspection equals the checked schema bytes.
    EXPECT_TRUE(bus_state.service->pairing_window_open());
    EXPECT_EQ(parse_string(introspect.value()), aa::ipc::introspection_xml());
    EXPECT_EQ(bus_state.service->state().projection, ProjectionState::stopped);
}

TEST(BusBoundary, UnauthorizedCallerIsRejectedThroughTheBoundary) {
    // Given: a registered consumer (first client) and a second same-UID
    // connection that owns no consumer name.
    BoundaryBus bus_state;
    bus_state.start();
    bus::MessageWriter register_body;
    register_body.body_signature("s").put_string(kConsumerName);
    ASSERT_TRUE(
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "RegisterConsumer", register_body)
            .has_value());
    auto intruder = bus::BusConnection::connect_session();
    ASSERT_TRUE(intruder.has_value());

    // When: the intruder drives control and pairing calls.
    bus::MessageWriter empty;
    empty.body_signature("");
    bus::MessageWriter call = empty;
    call.destination(kReceiverName)
        .path(kReceiverPath)
        .interface_name("org.custom.AndroidAutoReceiver1")
        .member("StartProjection");
    const auto serial = intruder.value().send_message(bus::MessageType::method_call, call);
    ASSERT_TRUE(serial.has_value());
    auto incoming = bus_state.server->read_message();
    ASSERT_TRUE(incoming.has_value());
    const auto call_view = bus::to_incoming_call(incoming.value());
    ASSERT_EQ(call_view.sender, intruder.value().unique_name());
    const auto reply = bus_state.dispatcher->handle(call_view);
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(bus_state.server
                    ->send_reply(incoming.value().serial, call_view.sender, *reply)
                    .has_value());
    const auto answer = intruder.value().await_reply(serial.value(), bus_state.server->unique_name());

    // Then: the daemon-stamped intruder identity is rejected by authorization.
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer.value().type, bus::MessageType::error_reply);
    EXPECT_EQ(answer.value().error_name, "org.custom.AndroidAutoReceiver1.Error.PeerUnauthorized");
}

TEST(BusBoundary, UnknownContractMajorIsRejectedThroughTheBoundary) {
    // Given: a running receiver boundary.
    BoundaryBus bus_state;
    bus_state.start();

    // When: a real call arrives on the v2 interface name.
    bus::MessageWriter empty;
    empty.body_signature("");
    bus::MessageWriter call = empty;
    call.destination(kReceiverName)
        .path(kReceiverPath)
        .interface_name("org.custom.AndroidAutoReceiver2")
        .member("StartProjection");
    const auto serial = bus_state.client->send_message(bus::MessageType::method_call, call);
    ASSERT_TRUE(serial.has_value());
    auto incoming = bus_state.server->read_message();
    ASSERT_TRUE(incoming.has_value());
    const auto call_view = bus::to_incoming_call(incoming.value());
    const auto reply = bus_state.dispatcher->handle(call_view);
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(bus_state.server
                    ->send_reply(incoming.value().serial, call_view.sender, *reply)
                    .has_value());
    const auto answer =
        bus_state.client->await_reply(serial.value(), bus_state.server->unique_name());

    // Then: the unknown major is rejected with the typed error name.
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer.value().type, bus::MessageType::error_reply);
    EXPECT_EQ(answer.value().error_name,
              "org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor");
}

TEST(BusBoundary, SignalWithControlMemberCannotChangeState) {
    // Given: a running receiver boundary.
    BoundaryBus bus_state;
    bus_state.start();

    // When: a SIGNAL (not a method call) carries the StartProjection member on
    // the real contract interface and object path.
    bus::MessageWriter signal;
    signal.destination(kReceiverName)
        .path(kReceiverPath)
        .interface_name("org.custom.AndroidAutoReceiver1")
        .member("StartProjection")
        .body_signature("");
    const auto serial = bus_state.client->send_message(bus::MessageType::signal, signal);
    ASSERT_TRUE(serial.has_value());
    auto incoming = bus_state.server->read_message();
    ASSERT_TRUE(incoming.has_value());
    const auto call_view = bus::to_incoming_call(incoming.value());
    const auto reply = bus_state.dispatcher->handle(call_view);
    const auto answer =
        bus_state.client->await_reply(serial.value(), bus_state.server->unique_name());

    // Then: the signal is dropped unexecuted — no reply, no dispatch, and the
    // projection state is untouched.
    EXPECT_FALSE(reply.has_value());
    EXPECT_FALSE(answer.has_value());
    EXPECT_EQ(bus_state.service->state().projection, ProjectionState::stopped);
}

TEST(BusBoundary, WrongPathMethodCallCannotChangeState) {
    // Given: a running receiver boundary.
    BoundaryBus bus_state;
    bus_state.start();

    // When: a real method call names StartProjection on a foreign object path.
    bus::MessageWriter call;
    call.destination(kReceiverName)
        .path("/org/custom/NotReceiver")
        .interface_name("org.custom.AndroidAutoReceiver1")
        .member("StartProjection")
        .body_signature("");
    const auto serial = bus_state.client->send_message(bus::MessageType::method_call, call);
    ASSERT_TRUE(serial.has_value());
    auto incoming = bus_state.server->read_message();
    ASSERT_TRUE(incoming.has_value());
    const auto call_view = bus::to_incoming_call(incoming.value());
    const auto reply = bus_state.dispatcher->handle(call_view);
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(bus_state.server
                    ->send_reply(incoming.value().serial, call_view.sender, *reply)
                    .has_value());
    const auto answer =
        bus_state.client->await_reply(serial.value(), bus_state.server->unique_name());

    // Then: the path gate answers with the typed UnknownObject rejection and
    // projection never starts.
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer.value().type, bus::MessageType::error_reply);
    EXPECT_EQ(answer.value().error_name, "org.freedesktop.DBus.Error.UnknownObject");
    EXPECT_EQ(bus_state.service->state().projection, ProjectionState::stopped);
}

} // namespace
