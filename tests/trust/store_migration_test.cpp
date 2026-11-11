// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 store migration: schema_version 1 (legacy key-only records) loads by
// migrating to opaque identities and persists as version 2; malformed legacy
// records are rejected and never silently trusted.
#include <aa/trust/Store.hpp>

#include "fakes.hpp"

#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
using aa::ErrorCode;

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path};
    out << text;
    (void)::chmod(path.c_str(), 0600);
}

TEST(StoreMigration, LegacyV1RecordsMigrateToOpaqueIdentities) {
    test::TempStoreDir dir;
    write_text(dir.file(),
               "{\"schema_version\":1,\"phones\":["
               "{\"phone_id\":7,\"key\":\"legacy-key-alpha\"},"
               "{\"phone_id\":9,\"key\":\"legacy-key-beta\"}]}");

    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_EQ(store.phones().size(), 2U);
    EXPECT_EQ(store.phones()[0].phone_id, aa::core::PhoneId{7});
    EXPECT_EQ(store.phones()[1].phone_id, aa::core::PhoneId{9});

    const trust::PhoneIdentity legacy{"legacy-key-beta"};
    EXPECT_EQ(store.lookup(legacy), trust::Decision::approved);
    EXPECT_EQ(store.lookup(trust::PhoneIdentity{"legacy-key-gamma"}), trust::Decision::unknown);
}

TEST(StoreMigration, MigratedStorePersistsAsCurrentVersion) {
    test::TempStoreDir dir;
    write_text(dir.file(),
               "{\"schema_version\":1,\"phones\":[{\"phone_id\":4,\"key\":\"legacy-key\"}]}");

    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(trust::PhoneIdentity{"legacy-key"}), trust::Decision::approved);
    EXPECT_EQ(fresh.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
    EXPECT_EQ(fresh.phones().size(), 2U);
    EXPECT_EQ(fresh.phones()[1].phone_id, aa::core::PhoneId{5});
}

TEST(StoreMigration, LegacyRecordWithoutKeyIsRejected) {
    test::TempStoreDir dir;
    write_text(dir.file(),
               "{\"schema_version\":1,\"phones\":[{\"phone_id\":1}]}");

    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_FALSE(store.usable());
}

TEST(StoreMigration, LegacyRecordWithUnexpectedKeyIsRejected) {
    test::TempStoreDir dir;
    write_text(dir.file(),
               "{\"schema_version\":1,\"phones\":["
               "{\"phone_id\":1,\"key\":\"k\",\"extra\":true}]}");

    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
}

TEST(StoreMigration, DuplicatePhoneIdIsRejected) {
    test::TempStoreDir dir;
    write_text(dir.file(),
               "{\"schema_version\":1,\"phones\":["
               "{\"phone_id\":3,\"key\":\"a\"},{\"phone_id\":3,\"key\":\"b\"}]}");

    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
}

TEST(StoreMigration, V2PortOnlyApprovalRequiresFreshConsent) {
    test::TempStoreDir dir;
    write_text(dir.file(),
        R"({"schema_version":2,"next_phone_id":8,"phones":[{"phone_id":7,"identity":{"kind":"wired","usb_vendor_id":"18d1","usb_product_id":"4ee1","usb_serial":"","aoa_manufacturer":"","aoa_model":"","aoa_serial":"","port_context":"1-2.3"}}]})");
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    EXPECT_TRUE(store.phones().empty());
    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(approved.has_value());
    EXPECT_EQ(approved.value(), aa::core::PhoneId{8});
    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    ASSERT_EQ(fresh.phones().size(), 1U);
    EXPECT_EQ(fresh.phones()[0].strength, trust::IdentityStrength::serial_backed);
}

TEST(StoreMigration, V3RejectsMissingOrMisrepresentedStrength) {
    test::TempStoreDir dir;
    for (const std::string tier : {"", ",\"strength\":\"serial-backed\"",
                                  ",\"strength\":\"unrecognized\""}) {
        write_text(dir.file(),
            "{\"schema_version\":3,\"next_phone_id\":2,\"phones\":[{\"phone_id\":1" + tier +
            R"(,"identity":{"kind":"wired","usb_vendor_id":"18d1","usb_product_id":"4ee1","usb_serial":"","aoa_manufacturer":"","aoa_model":"","aoa_serial":"","port_context":"1-2.3"}}]})");
        trust::ApprovedPhoneStore store{dir.file()};
        EXPECT_FALSE(store.load().has_value());
        EXPECT_FALSE(store.usable());
    }
}

TEST(StoreMigration, LegacyOpaquePortOnlyKeysCannotPreserveImplicitConsent) {
    for (int version : {1, 2}) {
        test::TempStoreDir dir;
        const trust::TransportIdentity weak{trust::WiredIdentity{
            "18d1", "4ee1", "", "", "", "", "1-2.3"}};
        const auto key = trust::identity_key(weak);
        const auto record = version == 1
            ? "{\"phone_id\":1,\"key\":\"" + key + "\"}"
            : "{\"phone_id\":1,\"identity\":{\"kind\":\"opaque\",\"key\":\"" + key + "\"}}";
        write_text(dir.file(), "{\"schema_version\":" + std::to_string(version) +
            (version == 2 ? ",\"next_phone_id\":2" : "") + ",\"phones\":[" + record + "]}");
        trust::ApprovedPhoneStore store{dir.file()};
        ASSERT_TRUE(store.load().has_value());
        EXPECT_TRUE(store.phones().empty());
        EXPECT_EQ(store.lookup(trust::PhoneIdentity{key}), trust::Decision::unknown);
    }
}

} // namespace
