// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 store schema hardening (gate-fix round 2): invalid documents are
// rejected despite the strict fail-closed schema — duplicate canonical
// identities (both versions), conflicting or out-of-range phone-id counters,
// inexact record members, unvalidated legacy keys and non-RFC-8259 integer
// spellings all load as typed trust_store_io rejections.
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

void expect_rejected(const std::string& text) {
    test::TempStoreDir dir;
    write_text(dir.file(), text);
    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_FALSE(store.usable());
}

TEST(StoreSchema, DuplicateCanonicalIdentityIsRejectedInV2) {
    expect_rejected(
        "{\"schema_version\":2,\"next_phone_id\":3,\"phones\":["
        "{\"phone_id\":1,\"identity\":{\"kind\":\"opaque\",\"key\":\"dup-key\"}},"
        "{\"phone_id\":2,\"identity\":{\"kind\":\"opaque\",\"key\":\"dup-key\"}}]}");
}

TEST(StoreSchema, DuplicateCanonicalIdentityIsRejectedInV1) {
    expect_rejected(
        "{\"schema_version\":1,\"phones\":["
        "{\"phone_id\":1,\"key\":\"dup-key\"},"
        "{\"phone_id\":2,\"key\":\"dup-key\"}]}");
}

TEST(StoreSchema, ConflictingCounterIsRejected) {
    expect_rejected(
        "{\"schema_version\":2,\"next_phone_id\":1,\"phones\":["
        "{\"phone_id\":1,\"identity\":{\"kind\":\"opaque\",\"key\":\"k\"}}]}");
}

TEST(StoreSchema, CounterPastCoherentRangeIsRejected) {
    expect_rejected(
        "{\"schema_version\":2,\"next_phone_id\":4098,\"phones\":[]}");
}

TEST(StoreSchema, RecordIdAboveCeilingIsRejected) {
    expect_rejected(
        "{\"schema_version\":2,\"next_phone_id\":4098,\"phones\":["
        "{\"phone_id\":4097,\"identity\":{\"kind\":\"opaque\",\"key\":\"k\"}}]}");
}

TEST(StoreSchema, LegacyRecordIdAboveCeilingIsRejected) {
    expect_rejected(
        "{\"schema_version\":1,\"phones\":["
        "{\"phone_id\":4097,\"key\":\"k\"}]}");
}

TEST(StoreSchema, CounterAtCoherentEdgeIsAccepted) {
    test::TempStoreDir dir;
    write_text(dir.file(), "{\"schema_version\":2,\"next_phone_id\":4097,\"phones\":[]}");
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    EXPECT_TRUE(store.usable());
}

TEST(StoreSchema, V2RecordWithUnexpectedMemberIsRejected) {
    expect_rejected(
        "{\"schema_version\":2,\"next_phone_id\":2,\"phones\":["
        "{\"phone_id\":1,\"note\":\"x\",\"identity\":{\"kind\":\"opaque\",\"key\":\"k\"}}]}");
}

TEST(StoreSchema, V2RecordWithMissingMemberIsRejected) {
    expect_rejected(
        "{\"schema_version\":2,\"next_phone_id\":2,\"phones\":["
        "{\"phone_id\":1}]}");
}

TEST(StoreSchema, LegacyRecordWithEmptyKeyIsRejected) {
    expect_rejected(
        "{\"schema_version\":1,\"phones\":[{\"phone_id\":1,\"key\":\"\"}]}");
}

TEST(StoreSchema, LegacyRecordWithUnprintableKeyIsRejected) {
    expect_rejected(
        "{\"schema_version\":1,\"phones\":[{\"phone_id\":1,\"key\":\"a\\nb\"}]}");
}

TEST(StoreSchema, LeadingZeroIntegerIsRejected) {
    expect_rejected(
        "{\"schema_version\":02,\"next_phone_id\":1,\"phones\":[]}");
    expect_rejected(
        "{\"schema_version\":2,\"next_phone_id\":01,\"phones\":[]}");
}

} // namespace
