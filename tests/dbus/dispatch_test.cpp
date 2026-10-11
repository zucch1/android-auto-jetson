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

void pad_to(std::vector<std::uint8_t>& body, std::size_t boundary) {
    while (body.size() % boundary != 0) {
        body.push_back(0);
    }
}

void append_u32(std::vector<std::uint8_t>& body, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        body.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void append_string(std::vector<std::uint8_t>& body, std::string_view value) {
    pad_to(body, 4);
    append_u32(body, static_cast<std::uint32_t>(value.size()));
    body.insert(body.end(), value.begin(), value.end());
    body.push_back(0);
}

void append_signature(std::vector<std::uint8_t>& body, std::string_view value) {
    body.push_back(static_cast<std::uint8_t>(value.size()));
    body.insert(body.end(), value.begin(), value.end());
    body.push_back(0);
}

void append_string_variant(std::vector<std::uint8_t>& body, std::string_view value) {
    append_signature(body, "s");
    append_string(body, value);
}

[[nodiscard]] std::vector<std::uint8_t> marshal_string(std::string_view value) {
    std::vector<std::uint8_t> body;
    append_string(body, value);
    return body;
}

[[nodiscard]] std::vector<std::uint8_t> marshal_u64(std::uint64_t value) {
    std::vector<std::uint8_t> body;
    for (int shift = 0; shift < 64; shift += 8) {
        body.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    return body;
}

[[nodiscard]] IncomingCall properties_call_from(std::string_view sender, std::string_view member,
                                                std::string_view signature,
                                                std::vector<std::uint8_t> body = {}) {
    return IncomingCall{BusName{std::string{sender}}, WireType::method_call,
                        std::string{aa::ipc::kContractObjectPath},
                        "org.freedesktop.DBus.Properties", std::string{member},
                        std::string{signature}, std::move(body)};
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

TEST(CallBoundary, PropertiesGetReturnsStringVariant) {
    // Given: a boundary over a live service with NO registered consumer.
    BoundaryFixture fixture;

    // When: Properties.Get asks for ContractSemVer on the contract interface.
    std::vector<std::uint8_t> body;
    append_string(body, "org.custom.AndroidAutoReceiver1");
    append_string(body, "ContractSemVer");
    const auto reply = fixture.dispatcher.handle(
        properties_call_from(":1.50", "Get", "ss", std::move(body)));

    // Then: the public property read answers the SemVer as the v-typed shape,
    // with no registration and no control authority involved.
    ASSERT_TRUE(reply.has_value());
    ASSERT_FALSE(reply->is_error);
    ASSERT_TRUE(std::holds_alternative<aa::ipc::StringPropertyValue>(reply->body));
    EXPECT_EQ(std::get<aa::ipc::StringPropertyValue>(reply->body).value, "1.0.0");
    EXPECT_EQ(fixture.service.state().consumer, aa::core::ConsumerId{});
}

TEST(CallBoundary, PropertiesGetAllReturnsContractMetadata) {
    // Given: a boundary over a live service with NO registered consumer.
    BoundaryFixture fixture;

    // When: Properties.GetAll targets the empty interface (the sole
    // property-bearing interface) and the contract interface.
    std::vector<std::uint8_t> empty_interface;
    append_string(empty_interface, "");
    const auto resolved = fixture.dispatcher.handle(
        properties_call_from(":1.51", "GetAll", "s", std::move(empty_interface)));
    std::vector<std::uint8_t> named_interface;
    append_string(named_interface, "org.custom.AndroidAutoReceiver1");
    const auto named = fixture.dispatcher.handle(
        properties_call_from(":1.51", "GetAll", "s", std::move(named_interface)));

    // Then: both answer the a{sv} property dictionary with the SemVer entry —
    // never the a{ss} capability-map shape.
    for (const auto* reply : {&resolved, &named}) {
        ASSERT_TRUE(reply->has_value());
        ASSERT_FALSE((*reply)->is_error);
        ASSERT_TRUE(std::holds_alternative<aa::ipc::PropertyDictionary>((*reply)->body));
        const auto& dictionary = std::get<aa::ipc::PropertyDictionary>((*reply)->body);
        ASSERT_EQ(dictionary.entries.size(), 1U);
        EXPECT_EQ(dictionary.entries.at("ContractSemVer"), "1.0.0");
    }
}

TEST(CallBoundary, PropertiesSetRejectsReadOnlyProperty) {
    // Given: a boundary over a live service.
    BoundaryFixture fixture;

    // When: Properties.Set targets ContractSemVer with a well-formed string
    // variant.
    std::vector<std::uint8_t> body;
    append_string(body, "org.custom.AndroidAutoReceiver1");
    append_string(body, "ContractSemVer");
    append_string_variant(body, "2.0.0");
    const auto reply = fixture.dispatcher.handle(
        properties_call_from(":1.52", "Set", "ssv", std::move(body)));

    // Then: the well-formed set attempt is rejected with the typed read-only
    // error and the contract version never changes.
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(reply->is_error);
    EXPECT_EQ(reply->error_name, "org.freedesktop.DBus.Error.PropertyReadOnly");
    EXPECT_EQ(aa::ipc::contract_semver(), "1.0.0");
}

TEST(CallBoundary, PropertiesRejectInvalidArguments) {
    // Given: a boundary over a live service.
    BoundaryFixture fixture;

    // When: outer signatures, strings and variants are malformed or carry
    // trailing bytes.
    std::vector<std::uint8_t> one_string;
    append_string(one_string, "org.custom.AndroidAutoReceiver1");
    const auto missing_property =
        fixture.dispatcher.handle(properties_call_from(":1.53", "Get", "s", one_string));
    const auto empty_signature =
        fixture.dispatcher.handle(properties_call_from(":1.53", "Get", "", {}));
    std::vector<std::uint8_t> three_strings;
    append_string(three_strings, "org.custom.AndroidAutoReceiver1");
    append_string(three_strings, "ContractSemVer");
    append_string(three_strings, "extra");
    const auto extra_signature = fixture.dispatcher.handle(
        properties_call_from(":1.53", "Get", "sss", std::move(three_strings)));
    std::vector<std::uint8_t> truncated_string{0x40, 0x00, 0x00, 0x00};
    const auto truncated = fixture.dispatcher.handle(
        properties_call_from(":1.53", "Get", "ss", std::move(truncated_string)));
    std::vector<std::uint8_t> trailing;
    append_string(trailing, "org.custom.AndroidAutoReceiver1");
    append_string(trailing, "ContractSemVer");
    trailing.push_back(0xAA);
    const auto trailing_bytes =
        fixture.dispatcher.handle(properties_call_from(":1.53", "Get", "ss", std::move(trailing)));
    std::vector<std::uint8_t> wrong_variant_type;
    append_string(wrong_variant_type, "org.custom.AndroidAutoReceiver1");
    append_string(wrong_variant_type, "ContractSemVer");
    append_signature(wrong_variant_type, "u");
    append_u32(wrong_variant_type, 7);
    const auto wrong_type = fixture.dispatcher.handle(
        properties_call_from(":1.53", "Set", "ssv", std::move(wrong_variant_type)));
    std::vector<std::uint8_t> truncated_variant;
    append_string(truncated_variant, "org.custom.AndroidAutoReceiver1");
    append_string(truncated_variant, "ContractSemVer");
    append_signature(truncated_variant, "s");
    const auto no_variant_value = fixture.dispatcher.handle(
        properties_call_from(":1.53", "Set", "ssv", std::move(truncated_variant)));

    // Then: every malformed shape is its own InvalidArgs rejection.
    for (const auto* reply : {&missing_property, &empty_signature, &extra_signature, &truncated,
                              &trailing_bytes, &wrong_type, &no_variant_value}) {
        ASSERT_TRUE(reply->has_value());
        EXPECT_TRUE((*reply)->is_error);
        EXPECT_EQ((*reply)->error_name, "org.freedesktop.DBus.Error.InvalidArgs");
    }
}

TEST(CallBoundary, PropertiesRejectUnknownTargets) {
    // Given: a boundary over a live service.
    BoundaryFixture fixture;

    // When: unknown properties, foreign interfaces, unsupported majors and
    // decimal-overflow majors are targeted.
    auto get = [&fixture](std::string_view interface_name, std::string_view property) {
        std::vector<std::uint8_t> body;
        append_string(body, interface_name);
        append_string(body, property);
        return fixture.dispatcher.handle(
            properties_call_from(":1.54", "Get", "ss", std::move(body)));
    };
    const auto unknown_property = get("org.custom.AndroidAutoReceiver1", "Nope");
    const auto foreign = get("org.example.Other", "ContractSemVer");
    const auto next_major = get("org.custom.AndroidAutoReceiver2", "ContractSemVer");
    const auto overflow_major =
        get("org.custom.AndroidAutoReceiver4294967297", "ContractSemVer");

    // Then: each target violation is its own typed rejection.
    ASSERT_TRUE(unknown_property.has_value());
    EXPECT_TRUE(unknown_property->is_error);
    EXPECT_EQ(unknown_property->error_name, "org.freedesktop.DBus.Error.UnknownProperty");
    ASSERT_TRUE(foreign.has_value());
    EXPECT_TRUE(foreign->is_error);
    EXPECT_EQ(foreign->error_name, "org.freedesktop.DBus.Error.UnknownInterface");
    for (const auto* reply : {&next_major, &overflow_major}) {
        ASSERT_TRUE(reply->has_value());
        EXPECT_TRUE((*reply)->is_error);
        EXPECT_EQ((*reply)->error_name,
                  "org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor");
    }
}

TEST(CallBoundary, PropertiesPreserveEnvelopeGates) {
    // Given: a boundary over a live service with no registration.
    BoundaryFixture fixture;

    // When: Properties-Get-shaped traffic arrives as non-method-call messages
    // and as a method call on a foreign object path.
    std::vector<std::uint8_t> body;
    append_string(body, "org.custom.AndroidAutoReceiver1");
    append_string(body, "ContractSemVer");
    for (const WireType type : {WireType::signal, WireType::method_return, WireType::error_reply}) {
        IncomingCall call{BusName{":1.55"}, type, std::string{aa::ipc::kContractObjectPath},
                          "org.freedesktop.DBus.Properties", "Get", "ss", body};
        EXPECT_FALSE(fixture.dispatcher.handle(call).has_value());
    }
    IncomingCall wrong_path{BusName{":1.55"}, WireType::method_call, "/org/custom/NotReceiver",
                            "org.freedesktop.DBus.Properties", "Get", "ss", body};
    const auto rejected = fixture.dispatcher.handle(wrong_path);

    // Then: the envelope gates still drop or reject before any Properties
    // handling runs.
    ASSERT_TRUE(rejected.has_value());
    EXPECT_TRUE(rejected->is_error);
    EXPECT_EQ(rejected->error_name, "org.freedesktop.DBus.Error.UnknownObject");
}

} // namespace
