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

} // namespace
