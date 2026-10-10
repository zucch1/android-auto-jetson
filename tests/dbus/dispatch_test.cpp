// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 call-dispatch boundary (gate round 1 finding 5, round 2 finding 1):
// caller identity comes EXCLUSIVELY from the daemon-stamped unique sender of
// the actual message, and message type, object path, interface, member and
// signature are validated against the checked contract before any argument is
// interpreted. Signals and other non-method-call traffic are dropped without
// dispatching control semantics; foreign object paths and unknown majors fail
// closed with typed error replies.
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/ControlDispatcher.hpp>
#include <aa/ipc/ControlService.hpp>

#include "fakes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using aa::ErrorCode;
using aa::ipc::BusName;
using aa::ipc::ControlDispatcher;
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;
using aa::ipc::IncomingCall;
using aa::ipc::OutboundReply;
using aa::ipc::ProjectionState;
using aa::ipc::WireType;

constexpr std::uint32_t kSessionUid = 1000;

struct BoundaryFixture final {
    aa::ipc::test::FakePeerCredentials credentials{};
    aa::ipc::test::RecordingEventSink events{};
    aa::ipc::test::SequencePairingIds ids{};
    aa::ipc::test::RecordingPhoneDirectory phones{};
    ControlService service;
    ControlDispatcher dispatcher;

    BoundaryFixture()
        : service(ControlServiceConfig{kSessionUid},
                  ControlServiceDeps{credentials, events, ids, phones}),
          dispatcher(service) {}
};

[[nodiscard]] IncomingCall call_from(std::string_view sender, std::string_view member,
                                     std::string_view signature = "",
                                     std::vector<std::uint8_t> body = {}) {
    return IncomingCall{BusName{std::string{sender}}, WireType::method_call,
                        std::string{aa::ipc::kContractObjectPath},
                        "org.custom.AndroidAutoReceiver1", std::string{member},
                        std::string{signature}, std::move(body)};
}

[[nodiscard]] std::vector<std::uint8_t> marshal_string(std::string_view value) {
    std::vector<std::uint8_t> body;
    const auto length = static_cast<std::uint32_t>(value.size());
    for (int shift = 0; shift < 32; shift += 8) {
        body.push_back(static_cast<std::uint8_t>((length >> shift) & 0xFFU));
    }
    body.insert(body.end(), value.begin(), value.end());
    body.push_back(0);
    return body;
}

[[nodiscard]] std::vector<std::uint8_t> marshal_u64(std::uint64_t value) {
    std::vector<std::uint8_t> body;
    for (int shift = 0; shift < 64; shift += 8) {
        body.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    return body;
}

TEST(CallBoundary, RejectsCallerWithoutDaemonStampedUniqueSender) {
    // Given: a boundary over a live service.
    BoundaryFixture fixture;

    // When: the sender is a well-known name or empty instead of a unique name.
    const auto named = fixture.dispatcher.handle(call_from("org.custom.ConsumerA", "Ping"));
    const auto missing = fixture.dispatcher.handle(call_from("", "Ping"));

    // Then: identity cannot be fabricated; both fail closed.
    ASSERT_TRUE(named.has_value());
    EXPECT_TRUE(named->is_error);
    EXPECT_EQ(named->error_name, "org.freedesktop.DBus.Error.Failed");
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(missing->is_error);
    EXPECT_EQ(missing->error_name, "org.freedesktop.DBus.Error.Failed");
}

TEST(CallBoundary, RejectsUnknownContractMajorVersions) {
    // Given: a boundary over a live service.
    BoundaryFixture fixture;

    // When: calls arrive on a v2 interface name and an overflowing one.
    auto v2 = call_from(":1.10", "StartProjection");
    v2.interface_name = "org.custom.AndroidAutoReceiver2";
    auto overflow = call_from(":1.10", "StartProjection");
    overflow.interface_name = "org.custom.AndroidAutoReceiver4294967297";

    // Then: both are rejected as unsupported contract majors.
    const auto rejected_v2 = fixture.dispatcher.handle(v2);
    const auto rejected_overflow = fixture.dispatcher.handle(overflow);
    ASSERT_TRUE(rejected_v2.has_value());
    EXPECT_TRUE(rejected_v2->is_error);
    EXPECT_EQ(rejected_v2->error_name,
              "org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor");
    ASSERT_TRUE(rejected_overflow.has_value());
    EXPECT_TRUE(rejected_overflow->is_error);
    EXPECT_EQ(rejected_overflow->error_name,
              "org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor");
}

TEST(CallBoundary, RejectsUnknownInterfaceUnknownMemberAndWrongSignature) {
    // Given: a boundary over a live service.
    BoundaryFixture fixture;

    // When: interface, member and signature each violate the checked schema.
    auto foreign = call_from(":1.10", "StartProjection");
    foreign.interface_name = "org.example.Other";
    const auto unknown_member = fixture.dispatcher.handle(call_from(":1.10", "Nope"));
    const auto wrong_signature =
        fixture.dispatcher.handle(call_from(":1.10", "RegisterConsumer", "ay"));

    // Then: each violation is its own typed rejection.
    const auto unknown_interface = fixture.dispatcher.handle(foreign);
    ASSERT_TRUE(unknown_interface.has_value());
    EXPECT_TRUE(unknown_interface->is_error);
    EXPECT_EQ(unknown_interface->error_name, "org.freedesktop.DBus.Error.UnknownInterface");
    ASSERT_TRUE(unknown_member.has_value());
    EXPECT_TRUE(unknown_member->is_error);
    EXPECT_EQ(unknown_member->error_name, "org.freedesktop.DBus.Error.UnknownMethod");
    ASSERT_TRUE(wrong_signature.has_value());
    EXPECT_TRUE(wrong_signature->is_error);
    EXPECT_EQ(wrong_signature->error_name, "org.freedesktop.DBus.Error.InvalidArgs");
}

TEST(CallBoundary, DerivesCallerIdentityFromSenderNotArguments) {
    // Given: a consumer name owned by connection :1.10 and a second
    // same-UID connection :1.11 that owns its own name but is not registered.
    BoundaryFixture fixture;
    const BusName name{"org.custom.ConsumerA"};
    const BusName intruder_name{"org.custom.ConsumerB"};
    fixture.credentials.add_connection(BusName{":1.10"}, kSessionUid);
    fixture.credentials.add_connection(BusName{":1.11"}, kSessionUid);
    fixture.credentials.set_owner(name, BusName{":1.10"});
    fixture.credentials.set_owner(intruder_name, BusName{":1.11"});

    // When: :1.10 registers and starts projection, then :1.11 tries both.
    const auto body = marshal_string(name.value);
    const auto registered =
        fixture.dispatcher.handle(call_from(":1.10", "RegisterConsumer", "s", body));
    const auto started = fixture.dispatcher.handle(call_from(":1.10", "StartProjection"));
    const auto intruder_register = fixture.dispatcher.handle(
        call_from(":1.11", "RegisterConsumer", "s", marshal_string(intruder_name.value)));
    const auto intruder_start = fixture.dispatcher.handle(call_from(":1.11", "StartProjection"));

    // Then: the daemon-stamped sender is the identity; the intruder is denied
    // regardless of any argument it could have supplied.
    ASSERT_TRUE(registered.has_value());
    EXPECT_FALSE(registered->is_error);
    ASSERT_TRUE(started.has_value());
    EXPECT_FALSE(started->is_error);
    ASSERT_TRUE(intruder_register.has_value());
    EXPECT_TRUE(intruder_register->is_error);
    EXPECT_EQ(intruder_register->error_name,
              "org.custom.AndroidAutoReceiver1.Error.ConsumerRejected");
    ASSERT_TRUE(intruder_start.has_value());
    EXPECT_TRUE(intruder_start->is_error);
    EXPECT_EQ(intruder_start->error_name,
              "org.custom.AndroidAutoReceiver1.Error.PeerUnauthorized");
}

TEST(CallBoundary, NonMethodCallMessagesAreDroppedNotDispatched) {
    // Given: a boundary over a live service with a registered consumer.
    BoundaryFixture fixture;
    const BusName name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(BusName{":1.40"}, kSessionUid);
    fixture.credentials.set_owner(name, BusName{":1.40"});
    ASSERT_TRUE(fixture.dispatcher
                    .handle(call_from(":1.40", "RegisterConsumer", "s", marshal_string(name.value)))
                    .has_value());

    // When: a signal, a method return and an error reply all name the
    // StartProjection member on the contract interface and path.
    for (const WireType type : {WireType::signal, WireType::method_return, WireType::error_reply}) {
        IncomingCall call{BusName{":1.40"}, type, std::string{aa::ipc::kContractObjectPath},
                          "org.custom.AndroidAutoReceiver1", "StartProjection", "", {}};
        const auto reply = fixture.dispatcher.handle(call);

        // Then: nothing is answered and projection never starts.
        EXPECT_FALSE(reply.has_value());
    }
    EXPECT_EQ(fixture.service.state().projection, ProjectionState::stopped);
}

TEST(CallBoundary, RejectsForeignObjectPathsWithoutDispatching) {
    // Given: a boundary over a live service with a registered consumer.
    BoundaryFixture fixture;
    const BusName name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(BusName{":1.41"}, kSessionUid);
    fixture.credentials.set_owner(name, BusName{":1.41"});
    ASSERT_TRUE(fixture.dispatcher
                    .handle(call_from(":1.41", "RegisterConsumer", "s", marshal_string(name.value)))
                    .has_value());

    // When: a real method call targets a foreign object path.
    IncomingCall call{BusName{":1.41"}, WireType::method_call,
                      "/org/custom/NotReceiver", "org.custom.AndroidAutoReceiver1",
                      "StartProjection", "", {}};
    const auto reply = fixture.dispatcher.handle(call);

    // Then: it is rejected typed and projection never starts.
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(reply->is_error);
    EXPECT_EQ(reply->error_name, "org.freedesktop.DBus.Error.UnknownObject");
    EXPECT_EQ(fixture.service.state().projection, ProjectionState::stopped);
}

TEST(CallBoundary, PairingFlowRoutesThroughTheBoundary) {
    // Given: a registered consumer.
    BoundaryFixture fixture;
    const BusName name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(BusName{":1.20"}, kSessionUid);
    fixture.credentials.set_owner(name, BusName{":1.20"});
    ASSERT_FALSE(fixture.dispatcher
                     .handle(call_from(":1.20", "RegisterConsumer", "s", marshal_string(name.value)))
                     ->is_error);

    // When: RequestAddPhone and ConfirmPhonePairing arrive as real calls.
    const auto request = fixture.dispatcher.handle(call_from(":1.20", "RequestAddPhone"));
    ASSERT_TRUE(request.has_value());
    ASSERT_FALSE(request->is_error);
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(request->body));
    const std::uint64_t request_id = std::get<std::uint64_t>(request->body);
    ASSERT_NE(request_id, 0U);
    EXPECT_FALSE(fixture.service.pairing_window_open());

    const auto confirmed = fixture.dispatcher.handle(
        call_from(":1.20", "ConfirmPhonePairing", "t", marshal_u64(request_id)));

    // Then: only the matching confirm opens the window.
    ASSERT_TRUE(confirmed.has_value());
    EXPECT_FALSE(confirmed->is_error);
    EXPECT_TRUE(fixture.service.pairing_window_open());
}

TEST(CallBoundary, IntrospectionServesCheckedSchemaBytes) {
    // Given: a boundary over a live service.
    BoundaryFixture fixture;

    // When: org.freedesktop.DBus.Introspectable.Introspect is called.
    IncomingCall introspect{BusName{":1.30"}, WireType::method_call,
                            std::string{aa::ipc::kContractObjectPath},
                            "org.freedesktop.DBus.Introspectable", "Introspect", "", {}};
    const auto reply = fixture.dispatcher.handle(introspect);

    // Then: the runtime serves exactly the checked schema XML.
    ASSERT_TRUE(reply.has_value());
    ASSERT_FALSE(reply->is_error);
    ASSERT_TRUE(std::holds_alternative<std::string>(reply->body));
    EXPECT_EQ(std::get<std::string>(reply->body), aa::ipc::introspection_xml());
}

} // namespace
