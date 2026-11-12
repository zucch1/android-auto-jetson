// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 call-dispatch boundary on a private bus (gate round 1, finding 5):
// real client method calls cross the actual bus, the daemon stamps the
// sender, and ControlDispatcher routes to ControlService authorization with
// that identity. RegisterConsumer/StartProjection/StopProjection, the pairing
// flow and introspection all run through the boundary; the caller never
// supplies its own identity.
#include <aa/ipc/BusPeerLookup.hpp>
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/ControlDispatcher.hpp>
#include <aa/ipc/ControlService.hpp>

#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include "../../src/ipc/dbus_adapter/BusIo.hpp"
#include "fakes.hpp"
#include "../../src/ipc/dbus_adapter/DropDiagnostic.hpp"

#include <array>
#include <chrono>
#include <thread>
#include <sys/socket.h>

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

[[nodiscard]] bus::MessageWriter oversized_ping() {
    bus::MessageWriter writer;
    writer.destination(kReceiverName)
        .path(kReceiverPath)
        .interface_name("org.custom.AndroidAutoReceiver1")
        .member("Ping")
        .body_signature("s")
        .put_string(std::string(70U * 1024U, 'x'));
    return writer;
}

void pad_to(std::vector<std::uint8_t>& bytes, std::size_t boundary) {
    while (bytes.size() % boundary != 0) {
        bytes.push_back(0);
    }
}

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_be_string_field(std::vector<std::uint8_t>& fields, std::uint8_t code,
                            std::string_view variant_type, std::string_view value) {
    pad_to(fields, 8);
    fields.push_back(code);
    fields.push_back(1);
    fields.push_back(static_cast<std::uint8_t>(variant_type.front()));
    fields.push_back(0);
    pad_to(fields, 4);
    append_be32(fields, static_cast<std::uint32_t>(value.size()));
    fields.insert(fields.end(), value.begin(), value.end());
    fields.push_back(0);
}

// A complete big-endian method call, assembled independently of MessageWriter
// so the daemon and receiver both see genuine big-endian framing and fields.
[[nodiscard]] std::vector<std::uint8_t> big_endian_ping(std::uint32_t serial) {
    std::vector<std::uint8_t> fields;
    append_be_string_field(fields, 1, "o", kReceiverPath);
    append_be_string_field(fields, 2, "s", "org.custom.AndroidAutoReceiver1");
    append_be_string_field(fields, 3, "s", "Ping");
    append_be_string_field(fields, 6, "s", kReceiverName);
    std::vector<std::uint8_t> bytes{'B', 1, 0, 1};
    append_be32(bytes, 0);
    append_be32(bytes, serial);
    append_be32(bytes, static_cast<std::uint32_t>(fields.size()));
    bytes.insert(bytes.end(), fields.begin(), fields.end());
    pad_to(bytes, 8);
    return bytes;
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
    // Given: a running receiver boundary with the consumer registered FIRST,
    // so the drop runs against an established session.
    BoundaryBus bus_state;
    bus_state.start();
    bus::MessageWriter register_body;
    register_body.body_signature("s").put_string(kConsumerName);
    const auto registered =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "RegisterConsumer", register_body);
    ASSERT_TRUE(registered.has_value());
    ASSERT_EQ(registered.value().type, bus::MessageType::method_return);
    // The identity issued at registration. A successful StartProjection alone would also
    // pass if the rejected traffic had swapped in a different consumer on this connection.
    const auto original_consumer = bus_state.service->state().consumer;
    ASSERT_NE(original_consumer, aa::core::ConsumerId{});

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

    // And: the drop leaves the transport synchronized — a subsequent Ping
    // round-trips to normal completion and does not wedge the connection.
    bus::MessageWriter ping_body;
    ping_body.body_signature("");
    const auto ping =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "Ping", ping_body);
    ASSERT_TRUE(ping.has_value());
    ASSERT_EQ(ping.value().type, bus::MessageType::method_return);
    ASSERT_EQ(ping.value().signature, "t");
    bus::Cursor ping_cursor(ping.value().body);
    const auto ping_payload = ping_cursor.read_u64();
    ASSERT_TRUE(ping_payload.has_value());
    EXPECT_TRUE(ping_cursor.done());

    // And: the original consumer registration is preserved and still holds authority.
    EXPECT_EQ(bus_state.service->state().consumer, original_consumer);
    bus::MessageWriter empty;
    empty.body_signature("");
    const auto start =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "StartProjection", empty);
    ASSERT_TRUE(start.has_value());
    EXPECT_EQ(start.value().type, bus::MessageType::method_return);
    EXPECT_EQ(bus_state.service->state().projection, ProjectionState::projecting);

    const auto stop =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "StopProjection", empty);
    ASSERT_TRUE(stop.has_value());
    EXPECT_EQ(stop.value().type, bus::MessageType::method_return);
    EXPECT_EQ(bus_state.service->state().projection, ProjectionState::stopped);
}

TEST(BusBoundary, WrongPathMethodCallCannotChangeState) {
    // Given: a running receiver boundary with the consumer registered FIRST,
    // so the rejection runs against an established session.
    BoundaryBus bus_state;
    bus_state.start();
    bus::MessageWriter register_body;
    register_body.body_signature("s").put_string(kConsumerName);
    const auto registered =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "RegisterConsumer", register_body);
    ASSERT_TRUE(registered.has_value());
    ASSERT_EQ(registered.value().type, bus::MessageType::method_return);
    // The identity issued at registration. A successful StartProjection alone would also
    // pass if the rejected traffic had swapped in a different consumer on this connection.
    const auto original_consumer = bus_state.service->state().consumer;
    ASSERT_NE(original_consumer, aa::core::ConsumerId{});

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

    // And: the rejection leaves the transport synchronized — a subsequent
    // Ping round-trips to normal completion and does not wedge the connection.
    bus::MessageWriter ping_body;
    ping_body.body_signature("");
    const auto ping =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "Ping", ping_body);
    ASSERT_TRUE(ping.has_value());
    ASSERT_EQ(ping.value().type, bus::MessageType::method_return);
    ASSERT_EQ(ping.value().signature, "t");
    bus::Cursor ping_cursor(ping.value().body);
    const auto ping_payload = ping_cursor.read_u64();
    ASSERT_TRUE(ping_payload.has_value());
    EXPECT_TRUE(ping_cursor.done());

    // And: the original consumer registration is preserved and still holds authority.
    EXPECT_EQ(bus_state.service->state().consumer, original_consumer);
    bus::MessageWriter empty;
    empty.body_signature("");
    const auto start =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "StartProjection", empty);
    ASSERT_TRUE(start.has_value());
    EXPECT_EQ(start.value().type, bus::MessageType::method_return);
    EXPECT_EQ(bus_state.service->state().projection, ProjectionState::projecting);

    const auto stop =
        round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "StopProjection", empty);
    ASSERT_TRUE(stop.has_value());
    EXPECT_EQ(stop.value().type, bus::MessageType::method_return);
    EXPECT_EQ(bus_state.service->state().projection, ProjectionState::stopped);
}

TEST(BusBoundary, PropertiesGetUsesVariantWireEncoding) {
    // Given: a running receiver boundary and a real Properties.Get call.
    BoundaryBus bus_state;
    bus_state.start();
    bus::MessageWriter get_body;
    get_body.body_signature("ss")
        .put_string("org.custom.AndroidAutoReceiver1")
        .put_string("ContractSemVer");

    // When: the call crosses the real bus and the reply comes back.
    const auto reply =
        round_trip(bus_state, "org.freedesktop.DBus.Properties", "Get", get_body);

    // Then: the reply marshals as "v" containing "s" with the SemVer value,
    // byte-for-byte the spec layout (independent of the project writer).
    ASSERT_TRUE(reply.has_value());
    ASSERT_EQ(reply.value().type, bus::MessageType::method_return);
    ASSERT_EQ(reply.value().signature, "v");
    const std::vector<std::uint8_t> expected{0x01, 0x73, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
                                             0x31, 0x2e, 0x30, 0x2e, 0x30, 0x00};
    EXPECT_EQ(reply.value().body, expected);
    bus::Cursor cursor(reply.value().body);
    const auto contained = cursor.read_signature();
    const auto value = cursor.read_string();
    ASSERT_TRUE(contained.has_value());
    EXPECT_EQ(contained.value(), "s");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), "1.0.0");
    EXPECT_TRUE(cursor.done());
}

TEST(BusBoundary, PropertiesGetAllUsesVariantDictionaryEncoding) {
    // Given: a running receiver boundary and a real Properties.GetAll call.
    BoundaryBus bus_state;
    bus_state.start();
    bus::MessageWriter get_all_body;
    get_all_body.body_signature("s").put_string("org.custom.AndroidAutoReceiver1");

    // When: the call crosses the real bus and the reply comes back.
    const auto reply =
        round_trip(bus_state, "org.freedesktop.DBus.Properties", "GetAll", get_all_body);

    // Then: the reply marshals as a{sv} with the exact array length (excluding
    // the gap before the first entry), 8-aligned dict entry, string key and
    // string-variant value — byte-for-byte the spec layout.
    ASSERT_TRUE(reply.has_value());
    ASSERT_EQ(reply.value().type, bus::MessageType::method_return);
    ASSERT_EQ(reply.value().signature, "a{sv}");
    const std::vector<std::uint8_t> expected{
        0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x43,
        0x6f, 0x6e, 0x74, 0x72, 0x61, 0x63, 0x74, 0x53, 0x65, 0x6d, 0x56, 0x65, 0x72,
        0x00, 0x01, 0x73, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x31, 0x2e, 0x30,
        0x2e, 0x30, 0x00};
    EXPECT_EQ(reply.value().body, expected);
    bus::Cursor cursor(reply.value().body);
    const auto array_length = cursor.read_u32();
    ASSERT_TRUE(array_length.has_value());
    EXPECT_EQ(array_length.value(), 34U);
    EXPECT_EQ(cursor.position(), 4U);
    ASSERT_TRUE(cursor.align(8).has_value());
    EXPECT_EQ(cursor.position(), 8U);
    const auto key = cursor.read_string();
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key.value(), "ContractSemVer");
    const auto contained = cursor.read_signature();
    ASSERT_TRUE(contained.has_value());
    EXPECT_EQ(contained.value(), "s");
    const auto value = cursor.read_string();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), "1.0.0");
    EXPECT_TRUE(cursor.done());
}

TEST(BusBoundary, PropertiesSetReturnsPropertyReadOnly) {
    // Given: a running receiver boundary and a well-formed Properties.Set call.
    BoundaryBus bus_state;
    bus_state.start();
    bus::MessageWriter set_body;
    set_body.body_signature("ssv")
        .put_string("org.custom.AndroidAutoReceiver1")
        .put_string("ContractSemVer")
        .put_string_variant("2.0.0");

    // When: the set attempt crosses the real bus.
    const auto reply =
        round_trip(bus_state, "org.freedesktop.DBus.Properties", "Set", set_body);

    // Then: the reply is an actual error message with the exact read-only
    // error name.
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply.value().type, bus::MessageType::error_reply);
    EXPECT_EQ(reply.value().error_name, "org.freedesktop.DBus.Error.PropertyReadOnly");
    EXPECT_EQ(bus_state.service->capabilities().at("contract.semver"), "1.0.0");
}

TEST(BusBoundary, OversizeCallIsDrainedAndTheNextCallSurvives) {
    // Given: a real daemon willing to forward a call beyond the receiver's
    // deliberately smaller 64 KiB control-contract maximum.
    BoundaryBus bus_state;
    bus_state.start();
    const bus::MessageWriter oversized = oversized_ping();

    // When: the 70 KiB call is followed by an ordinary valid call.
    const auto sent = bus_state.client->send_message(bus::MessageType::method_call, oversized);
    ASSERT_TRUE(sent.has_value());
    const auto dropped = bus_state.server->read_message();

    // Then: the complete oversize frame is discarded as a distinct non-fatal
    // result, counted for diagnostics, and the stream starts at the next head.
    ASSERT_FALSE(dropped.has_value());
    EXPECT_EQ(dropped.error().code(), ErrorCode::transport_message_dropped);
    EXPECT_EQ(bus_state.server->dropped_messages(), 1U);
    EXPECT_GE(bus_state.server->native_handle(), 0);
    bus::MessageWriter ping_body;
    ping_body.body_signature("");
    const auto ping = round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "Ping", ping_body);
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping.value().type, bus::MessageType::method_return);
    EXPECT_EQ(ping.value().signature, "t");
}

TEST(BusBoundary, OversizeDropDoesNotStopTheControlService) {
    // Given: the production session-bus service rather than a manual dispatch
    // loop, so the caller's non-fatal handling is exercised too.
    aa::ipc::test::SequencePairingIds ids;
    aa::ipc::test::RecordingPhoneDirectory phones;
    auto service = aa::ipc::ControlBusService::start(
        {static_cast<std::uint32_t>(::getuid())}, {ids, phones});
    ASSERT_TRUE(service.has_value());
    auto client = bus::BusConnection::connect_session();
    ASSERT_TRUE(client.has_value());
    const auto oversized = client.value().send_message(bus::MessageType::method_call, oversized_ping());
    ASSERT_TRUE(oversized.has_value());

    // When/Then: a drop is a successful zero-dispatch cycle, not service loss.
    const auto dropped = service.value().cycle(aa::core::Milliseconds{1000});
    ASSERT_TRUE(dropped.has_value());
    EXPECT_EQ(dropped.value(), 0U);
    EXPECT_FALSE(service.value().closed());

    // And: the next valid call is dispatched and answered normally.
    bus::MessageWriter ping;
    ping.destination(kReceiverName)
        .path(kReceiverPath)
        .interface_name("org.custom.AndroidAutoReceiver1")
        .member("Ping")
        .body_signature("");
    const auto sent = client.value().send_message(bus::MessageType::method_call, ping);
    ASSERT_TRUE(sent.has_value());
    const auto dispatched = service.value().cycle(aa::core::Milliseconds{1000});
    ASSERT_TRUE(dispatched.has_value());
    EXPECT_EQ(dispatched.value(), 1U);
    const auto reply = client.value().await_reply(sent.value(), service.value().unique_name());
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply.value().type, bus::MessageType::method_return);
    EXPECT_EQ(reply.value().signature, "t");
}

TEST(BusBoundary, BigEndianCallIsDrainedAndTheNextCallSurvives) {
    // Given: a syntactically valid big-endian call accepted by the real bus.
    BoundaryBus bus_state;
    bus_state.start();
    const auto bytes = big_endian_ping(1000);
    const auto frame = bus::inspect_frame(bytes);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame.value().total_bytes, bytes.size());
    ASSERT_FALSE(frame.value().little_endian);

    // When: it precedes a little-endian call on the same client connection.
    const auto sent = bus::bus_io::write_full(bus_state.client->native_handle(), bytes);
    ASSERT_TRUE(sent.has_value());
    const auto dropped = bus_state.server->read_message();

    // Then: byte-order-aware framing drains exactly one message. The strict
    // little-endian value parser need not grow a second decoding mode.
    ASSERT_FALSE(dropped.has_value());
    EXPECT_EQ(dropped.error().code(), ErrorCode::transport_message_dropped);
    EXPECT_EQ(bus_state.server->dropped_messages(), 1U);
    bus::MessageWriter ping_body;
    ping_body.body_signature("");
    const auto ping = round_trip(bus_state, "org.custom.AndroidAutoReceiver1", "Ping", ping_body);
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping.value().type, bus::MessageType::method_return);
    EXPECT_EQ(ping.value().signature, "t");
}


TEST(BusBoundary, DropDiagnosticsAggregateAtMostOncePerInterval) {
    bus::DropDiagnostic throttle;
    const bus::DropDiagnostic::Clock::time_point start{};
    EXPECT_EQ(throttle.record(start), 1U);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(throttle.record(start + std::chrono::seconds(9)));
    }
    EXPECT_EQ(throttle.record(start + std::chrono::seconds(10)), 1001U);
    EXPECT_FALSE(throttle.record(start + std::chrono::seconds(10)));
    EXPECT_EQ(throttle.record(start + std::chrono::seconds(20)), 2U);
    EXPECT_EQ(throttle.record(start + std::chrono::seconds(100)), 1U);
}

// Substitute only the authenticated connection's transport, leaving production
// read_message ownership/closure intact and avoiding daemon frame rewriting.
class SocketBoundary : public ::testing::Test {
protected:
    std::optional<bus::BusConnection> receiver;
    int peer{-1};
    void SetUp() override {
        auto connected = bus::BusConnection::connect_session();
        ASSERT_TRUE(connected.has_value());
        receiver.emplace(std::move(connected.value()));
        int sockets[2]{};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        peer = sockets[1];
        ASSERT_EQ(::dup2(sockets[0], receiver->native_handle()), receiver->native_handle());
        ::close(sockets[0]);
    }
    void TearDown() override { if (peer >= 0) ::close(peer); }
    static std::vector<std::uint8_t> frame(std::size_t total) {
        bus::MessageWriter writer;
        writer.path(kReceiverPath).interface_name("org.custom.AndroidAutoReceiver1")
            .member("Ping").body_signature("s").put_string("x");
        auto bytes = writer.build(bus::MessageType::method_call, 42);
        const auto body = total - (bytes.size() - 6); // string length + x + NUL
        bytes.resize(total, 0);
        for (unsigned i = 0; i < 4; ++i) bytes[4 + i] = static_cast<std::uint8_t>((body >> (8U * i)) & 0xffU);
        return bytes;
    }
};

TEST_F(SocketBoundary, CapPlusOneDrainsExactlyAndParsesNextMessage) {
    const auto oversized = frame(bus::kMaxMessageBytes + 1);
    ASSERT_EQ(bus::inspect_frame(oversized).value().total_bytes, bus::kMaxMessageBytes + 1);
    bus::MessageWriter ping;
    ping.path(kReceiverPath).member("Ping");
    const auto valid = ping.build(bus::MessageType::method_call, 43);
    bool sent = false;
    std::jthread writer{[&] {
        sent = bus::bus_io::write_full(peer, oversized).has_value() &&
               bus::bus_io::write_full(peer, valid).has_value();
    }};
    const auto dropped = receiver->read_message();
    const auto next = receiver->read_message();
    writer.join();
    ASSERT_TRUE(sent);
    ASSERT_FALSE(dropped.has_value());
    EXPECT_EQ(dropped.error().code(), ErrorCode::transport_message_dropped);
    EXPECT_EQ(receiver->dropped_messages(), 1U);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value().serial, 43U);
    EXPECT_EQ(next.value().member, "Ping");
}

TEST_F(SocketBoundary, PeerClosingMidDrainClosesConnection) {
    const auto oversized = frame(1024 * 1024);
    ASSERT_TRUE(bus::bus_io::write_full(peer, std::span{oversized}.first(5000)));
    ::close(peer); peer = -1;
    const auto start = std::chrono::steady_clock::now();
    const auto result = receiver->read_message();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::transport_closed);
    EXPECT_EQ(receiver->native_handle(), -1);
    EXPECT_EQ(receiver->dropped_messages(), 0U);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST_F(SocketBoundary, TricklingPeerCannotExtendDrainDeadline) {
    const auto oversized = frame(1024 * 1024);
    ASSERT_TRUE(bus::bus_io::write_full(peer, std::span{oversized}.first(16)));
    std::jthread trickle{[&](const std::stop_token& stop) {
        const std::array<std::uint8_t, 1024> chunk{};
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
            if (!bus::bus_io::write_full(peer, chunk, until)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }};
    const auto start = std::chrono::steady_clock::now();
    receiver->deadline(start + std::chrono::milliseconds(100));
    const auto result = receiver->read_message();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    trickle.request_stop(); trickle.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code() == ErrorCode::transport_io ||
                result.error().code() == ErrorCode::timeout);
    EXPECT_EQ(receiver->native_handle(), -1);
    EXPECT_EQ(receiver->dropped_messages(), 0U);
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));
    EXPECT_LT(elapsed, std::chrono::milliseconds(400));
}

} // namespace
