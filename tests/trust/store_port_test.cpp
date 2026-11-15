// SPDX-License-Identifier: GPL-3.0-or-later
// #133 port-independent wired matching: a serial-backed wired approval admits
// the same phone on any host port, a port-only (serial-less) approval stays
// bound to its own port, and stores already on disk (v1 opaque keys, v2/v3
// structured records) keep admitting their phone without any rewrite.
#include <aa/trust/PairingPolicy.hpp>
#include <aa/trust/Store.hpp>

#include "fakes.hpp"

#include <sys/stat.h>

#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
using aa::ErrorCode;

trust::WiredIdentity on_port(trust::WiredIdentity identity, std::string port) {
    identity.port_context = std::move(port);
    return identity;
}

trust::PhoneIdentity presented(const trust::WiredIdentity& identity) {
    return test::identity_of(trust::TransportIdentity{identity});
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path};
    out << text;
    out.close();
    (void)::chmod(path.c_str(), 0600);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in{path};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

const trust::WiredIdentity kPortOnly{"04e8", "6860", "", "", "", "", "1-2.1"};

TEST(TrustStore, SerialBackedWiredApprovalAdmitsTheSamePhoneOnAnotherPort) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    const auto approved = store.approve(trust::TransportIdentity{on_port(test::kWired, "1-2.1")});
    ASSERT_TRUE(approved.has_value());

    for (const char* port : {"1-2.1", "1-2.3", "2-1", "1-2.4.1"}) {
        EXPECT_EQ(store.lookup(presented(on_port(test::kWired, port))), trust::Decision::approved)
            << port;
    }
    // Reloaded from disk, still port-independent; the recorded port is kept.
    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(presented(on_port(test::kWired, "1-2.3"))), trust::Decision::approved);
    ASSERT_EQ(fresh.phones().size(), 1U);
    EXPECT_EQ(std::get<trust::WiredIdentity>(fresh.phones()[0].identity).port_context, "1-2.1");

    // Re-approving on the new port is approve-once: same id, no second record.
    const auto again = store.approve(trust::TransportIdentity{on_port(test::kWired, "1-2.3")});
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again.value(), approved.value());
    EXPECT_EQ(store.phones().size(), 1U);
}

TEST(TrustStore, PortIndependenceNeverIgnoresAnyPhoneField) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
    const auto other_port = on_port(test::kWired, "1-2.4");
    for (std::string trust::WiredIdentity::*field :
         {&trust::WiredIdentity::usb_vendor_id, &trust::WiredIdentity::usb_product_id,
          &trust::WiredIdentity::usb_serial, &trust::WiredIdentity::aoa_manufacturer,
          &trust::WiredIdentity::aoa_model, &trust::WiredIdentity::aoa_serial}) {
        auto changed = other_port;
        changed.*field += "x";
        EXPECT_EQ(store.lookup(presented(changed)), trust::Decision::unknown);
        auto same_port = test::kWired;
        same_port.*field += "x";
        EXPECT_EQ(store.lookup(presented(same_port)), trust::Decision::unknown);
    }
}

TEST(TrustStore, PortOnlyApprovalIsNeverWidenedToAnotherPort) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve_port_only(kPortOnly).has_value());
    EXPECT_EQ(store.lookup(presented(kPortOnly)), trust::Decision::approved);
    for (const char* port : {"1-2.3", "1-2", "2-1", ""}) {
        EXPECT_EQ(store.lookup(presented(on_port(kPortOnly, port))), trust::Decision::unknown)
            << port;
    }
    // A second explicit port-only grant on another port is a distinct record.
    const auto second = store.approve_port_only(on_port(kPortOnly, "1-2.3"));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(store.phones().size(), 2U);

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(presented(on_port(kPortOnly, "1-2.4"))), trust::Decision::unknown);
    // Forgetting one port-only grant leaves the other port's grant alone.
    ASSERT_TRUE(fresh.forget_phone(second.value()).has_value());
    EXPECT_EQ(fresh.lookup(presented(on_port(kPortOnly, "1-2.3"))), trust::Decision::unknown);
    EXPECT_EQ(fresh.lookup(presented(kPortOnly)), trust::Decision::approved);
}

TEST(TrustStore, MatchKeyDropsOnlyTheSerialBoundWiredPort) {
    const auto serial = trust::identity_key(trust::TransportIdentity{test::kWired});
    const auto moved = trust::identity_key(trust::TransportIdentity{on_port(test::kWired, "2-1")});
    ASSERT_NE(serial, moved);
    EXPECT_EQ(trust::match_key(serial), trust::match_key(moved));
    EXPECT_EQ(trust::match_key(serial),
              trust::identity_key(trust::TransportIdentity{on_port(test::kWired, "")}));
    const auto weak = trust::identity_key(trust::TransportIdentity{kPortOnly});
    EXPECT_EQ(trust::match_key(weak), weak);
    const auto wireless = trust::identity_key(trust::TransportIdentity{test::kWireless});
    EXPECT_EQ(trust::match_key(wireless), wireless);
    for (const std::string& malformed :
         {std::string{"w1:"}, std::string{"w1:1:a"}, serial + "x", "w1:01:a" + serial.substr(6)}) {
        EXPECT_EQ(trust::match_key(malformed), malformed);
    }
    EXPECT_EQ(trust::match_key(std::string{"legacy-key"}), "legacy-key");
}

TEST(TrustStore, ForgetRevokesASerialBackedPhoneRecordedOnSeveralPorts) {
    // A store hand-edited before #133 can hold one phone twice (one record
    // per port). Both now match the same phone, so one forget revokes both.
    test::TempStoreDir dir;
    const auto record = [](int id, const char* port) {
        return "{\"phone_id\":" + std::to_string(id) +
               ",\"strength\":\"serial-backed\",\"identity\":{\"kind\":\"wired\","
               "\"usb_vendor_id\":\"18d1\",\"usb_product_id\":\"4ee1\",\"usb_serial\":\"SERIAL-42\","
               "\"aoa_manufacturer\":\"Google\",\"aoa_model\":\"Pixel\",\"aoa_serial\":\"AOA-42\","
               "\"port_context\":\"" + std::string{port} + "\"}}";
    };
    write_text(dir.file(), "{\"schema_version\":3,\"next_phone_id\":3,\"phones\":[" +
                               record(1, "1-2.1") + "," + record(2, "1-2.3") + "]}");
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    EXPECT_EQ(store.lookup(presented(on_port(test::kWired, "1-2.4"))), trust::Decision::approved);
    ASSERT_TRUE(store.forget_phone(aa::core::PhoneId{2}).has_value());
    EXPECT_TRUE(store.phones().empty());
    for (const char* port : {"1-2.1", "1-2.3", "1-2.4"}) {
        EXPECT_EQ(store.lookup(presented(on_port(test::kWired, port))), trust::Decision::unknown);
    }
}

TEST(TrustStore, PairingForgetInvalidatesAPendingRequestForTheSamePhoneOnAnotherPort) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};
    const auto phone = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(phone.has_value());
    policy.note_candidate(trust::TransportIdentity{on_port(test::kWired, "1-2.4")});
    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{11}).has_value());
    ASSERT_TRUE(policy.forget(phone.value()).has_value());
    EXPECT_FALSE(policy.has_pending(aa::core::PairingRequestId{11}));
    const auto invalidated = policy.take_invalidated_pairings();
    ASSERT_EQ(invalidated.size(), 1U);
    EXPECT_EQ(invalidated[0], aa::core::PairingRequestId{11});
}

// (c) Stores already on the owner's device keep admitting their phone: no
// schema bump, no rewrite on load, and the phone is admitted on a new port.
TEST(StoreMigration, ExistingV3WiredRecordAdmitsItsPhoneOnAnyPortWithoutRewrite) {
    test::TempStoreDir dir;
    const std::string document =
        R"({"schema_version":3,"next_phone_id":2,"phones":[{"phone_id":1,"identity":{"kind":"wired","usb_vendor_id":"04e8","usb_product_id":"6860","usb_serial":"R5CX00000000","aoa_manufacturer":"samsung","aoa_model":"SM-S928U","aoa_serial":"R5CX00000000","port_context":"1-2.1"},"strength":"serial-backed"}]})";
    write_text(dir.file(), document);
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    const trust::WiredIdentity phone{"04e8", "6860", "R5CX00000000", "samsung",
                                     "SM-S928U", "R5CX00000000", "1-2.1"};
    EXPECT_EQ(store.lookup(presented(phone)), trust::Decision::approved);
    EXPECT_EQ(store.lookup(presented(on_port(phone, "1-2.3"))), trust::Decision::approved);
    EXPECT_EQ(read_text(dir.file()), document);
}

TEST(StoreMigration, LegacyV1AndV2WiredRecordsAdmitTheirPhoneOnAnotherPort) {
    const auto key = trust::identity_key(trust::TransportIdentity{test::kWired});
    const std::string structured =
        R"({"kind":"wired","usb_vendor_id":"18d1","usb_product_id":"4ee1","usb_serial":"SERIAL-42","aoa_manufacturer":"Google","aoa_model":"Pixel","aoa_serial":"AOA-42","port_context":"1-2.3"})";
    for (const std::string& document : {
             std::string{"{\"schema_version\":1,\"phones\":[{\"phone_id\":5,\"key\":\""} + key +
                 "\"}]}",
             "{\"schema_version\":2,\"next_phone_id\":6,\"phones\":[{\"phone_id\":5,\"identity\":" +
                 structured + "}]}",
             "{\"schema_version\":2,\"next_phone_id\":6,\"phones\":[{\"phone_id\":5,\"identity\":"
             "{\"kind\":\"opaque\",\"key\":\"" + key + "\"}}]}"}) {
        test::TempStoreDir dir;
        write_text(dir.file(), document);
        trust::ApprovedPhoneStore store{dir.file()};
        ASSERT_TRUE(store.load().has_value()) << document;
        EXPECT_EQ(store.dropped_port_only_count(), 0U);
        EXPECT_EQ(store.lookup(presented(test::kWired)), trust::Decision::approved);
        EXPECT_EQ(store.lookup(presented(on_port(test::kWired, "1-2.1"))), trust::Decision::approved);
        // Approve-once across ports returns the migrated id.
        const auto again = store.approve(trust::TransportIdentity{on_port(test::kWired, "1-2.1")});
        ASSERT_TRUE(again.has_value());
        EXPECT_EQ(again.value(), aa::core::PhoneId{5});
        EXPECT_EQ(store.phones().size(), 1U);
    }
}

TEST(StoreMigration, ExistingV3PortOnlyRecordStaysOnItsPort) {
    test::TempStoreDir dir;
    write_text(dir.file(),
        R"({"schema_version":3,"next_phone_id":2,"phones":[{"phone_id":1,"identity":{"kind":"wired","usb_vendor_id":"04e8","usb_product_id":"6860","usb_serial":"","aoa_manufacturer":"","aoa_model":"","aoa_serial":"","port_context":"1-2.1"},"strength":"port-only"}]})");
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    EXPECT_EQ(store.lookup(presented(kPortOnly)), trust::Decision::approved);
    EXPECT_EQ(store.lookup(presented(on_port(kPortOnly, "1-2.3"))), trust::Decision::unknown);
}

} // namespace
