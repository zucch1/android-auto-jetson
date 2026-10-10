// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 forged-reply regression (gate round 1, finding 1): unsolicited
// method returns from another bus peer — even ones carrying the exact reply
// serial — must never be accepted as credential-query answers. Reply
// acceptance requires the daemon-stamped sender org.freedesktop.DBus, the
// expected reply serial AND the expected destination; forged UID and forged
// name-ownership replies cannot influence admission.
#include <aa/ipc/BusPeerLookup.hpp>
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
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;
namespace bus = aa::ipc::dbus_adapter;

constexpr const char* kConsumerName = "org.custom.AndroidAutoReceiverTest.Consumer";
constexpr std::uint32_t kForgedUid = 1042;

// Serials of the victim lookup connection are deterministic: Hello consumes
// serial 1, so the first credential query is serial 2 and the second is 3.
constexpr std::uint32_t kFirstQuerySerial = 2;
constexpr std::uint32_t kSecondQuerySerial = 3;

[[nodiscard]] bus::MessageWriter forged_u32_reply(std::uint32_t reply_serial,
                                                  const BusName& destination,
                                                  std::uint32_t value) {
    bus::MessageWriter writer;
    writer.reply_serial(reply_serial).destination(destination.value).body_signature("u");
    writer.put_u32(value);
    return writer;
}

[[nodiscard]] bus::MessageWriter forged_string_reply(std::uint32_t reply_serial,
                                                     const BusName& destination,
                                                     std::string_view value) {
    bus::MessageWriter writer;
    writer.reply_serial(reply_serial).destination(destination.value).body_signature("s");
    writer.put_string(value);
    return writer;
}

TEST(BusSpoof, ForgedUidReplyCannotInfluenceAdmission) {
    // Given: a production lookup whose bus connection is the victim, an
    // attacker connection, and a consumer owning its name on the bus.
    BusPeerLookup lookup;
    ASSERT_TRUE(lookup.connected());
    auto attacker = bus::BusConnection::connect_session();
    auto client = bus::BusConnection::connect_session();
    ASSERT_TRUE(attacker.has_value());
    ASSERT_TRUE(client.has_value());
    ASSERT_TRUE(client.value().request_name(BusName{kConsumerName}).has_value());

    // When: the attacker injects a forged UID reply for the upcoming query
    // BEFORE the legitimate daemon answer exists.
    const auto injected = attacker.value().send_message(
        bus::MessageType::method_return,
        forged_u32_reply(kFirstQuerySerial, lookup.unique_name(), kForgedUid));
    ASSERT_TRUE(injected.has_value());
    aa::ipc::test::RecordingEventSink events;
    aa::ipc::test::SequencePairingIds ids;
    aa::ipc::test::RecordingPhoneDirectory phones;
    ControlService service(ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
                           ControlServiceDeps{lookup, events, ids, phones});
    const auto registered =
        service.register_consumer(client.value().unique_name(), BusName{kConsumerName});

    // Then: admission uses the REAL daemon-stamped UID, never the forged one
    // (a believed 1042 would have failed the session-user check).
    ASSERT_TRUE(registered.has_value())
        << "forged uid must not influence admission: real uid answer wins";
    EXPECT_EQ(lookup.unix_uid(client.value().unique_name()).value(),
              static_cast<std::uint32_t>(::getuid()));
}

TEST(BusSpoof, ForgedOwnershipReplyCannotInfluenceAdmission) {
    // Given: a production lookup, an attacker and a client that owns nothing.
    BusPeerLookup lookup;
    ASSERT_TRUE(lookup.connected());
    auto attacker = bus::BusConnection::connect_session();
    auto client = bus::BusConnection::connect_session();
    ASSERT_TRUE(attacker.has_value());
    ASSERT_TRUE(client.has_value());

    // When: the attacker forges the owner answer for an unowned name so it
    // looks as if the client owns it (query serial 3: uid query is serial 2).
    const auto injected = attacker.value().send_message(
        bus::MessageType::method_return,
        forged_string_reply(kSecondQuerySerial, lookup.unique_name(),
                            client.value().unique_name().value));
    ASSERT_TRUE(injected.has_value());
    aa::ipc::test::RecordingEventSink events;
    aa::ipc::test::SequencePairingIds ids;
    aa::ipc::test::RecordingPhoneDirectory phones;
    ControlService service(ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
                           ControlServiceDeps{lookup, events, ids, phones});
    const auto registered = service.register_consumer(client.value().unique_name(),
                                                      BusName{"org.custom.NobodyHome"});

    // Then: real name ownership (nobody) wins and registration is rejected —
    // the forged owner answer was never accepted.
    ASSERT_FALSE(registered.has_value());
    EXPECT_EQ(registered.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(BusSpoof, ForgedOnlyReplySprayFailsClosedTyped) {
    // Given: a production lookup and an attacker flooding forged replies for
    // every upcoming credential query serial.
    BusPeerLookup lookup;
    ASSERT_TRUE(lookup.connected());
    auto attacker = bus::BusConnection::connect_session();
    ASSERT_TRUE(attacker.has_value());
    for (int spray = 0; spray < 32; ++spray) {
        ASSERT_TRUE(attacker
                        .value()
                        .send_message(bus::MessageType::method_return,
                                      forged_u32_reply(kFirstQuerySerial, lookup.unique_name(),
                                                       kForgedUid))
                        .has_value());
    }

    // When: the victim performs the real query with only forgeries ahead.
    const auto uid = lookup.unix_uid(BusName{"org.freedesktop.DBus"});

    // Then: the bounded reader rejects the forgeries and fails closed with a
    // typed error — the forged UID is never returned.
    ASSERT_FALSE(uid.has_value());
    EXPECT_EQ(uid.error().code(), ErrorCode::ipc_peer_unauthorized);
}

} // namespace
