// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 approved-phone store semantics: approve-once grants reconnect
// eligibility, forget revokes it and persists, unknown phones fail closed, and
// malformed store files are rejected without ever silently trusting content.
#include <aa/trust/Store.hpp>

#include "fakes.hpp"

#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
using aa::Error;
using aa::ErrorCode;

TEST(TrustStore, ApproveOnceGrantsReconnectEligibility) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(approved.has_value());
    EXPECT_EQ(approved.value(), aa::core::PhoneId{1});

    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
    EXPECT_TRUE(trust::allows_session(trust::Decision::approved));

    test::TempStoreDir reloaded_dir;
    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
}

TEST(TrustStore, ApproveOnceIsIdempotentForTheSameIdentity) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const auto first = store.approve(trust::TransportIdentity{test::kWired});
    const auto second = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(store.phones().size(), 1U);
}

TEST(TrustStore, ForgetRemovesReconnectEligibilityAndPersists) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(approved.has_value());

    ASSERT_TRUE(store.forget_phone(approved.value()).has_value());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
    EXPECT_FALSE(trust::allows_session(trust::Decision::unknown));

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
}

TEST(TrustStore, UnknownPhoneFailsClosed) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWireless})),
              trust::Decision::unknown);
    EXPECT_FALSE(trust::allows_session(trust::Decision::unknown));
}

TEST(TrustStore, WiredAndWirelessIdentitiesAreDistinctRecords) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWireless}).has_value());
    EXPECT_EQ(store.phones().size(), 2U);
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWireless})),
              trust::Decision::approved);
}

TEST(TrustStore, ForgetUnknownPhoneIsATypedError) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const auto missing = store.forget_phone(aa::core::PhoneId{42});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::trust_unknown_phone);
}

TEST(TrustStore, MalformedStoreIsRejectedAndNeverTrusted) {
    test::TempStoreDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    {
        std::ofstream out{dir.file()};
        out << "{not json at all";
    }
    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_FALSE(store.usable());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
    const auto approve = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_FALSE(approve.has_value());
    EXPECT_EQ(approve.error().code(), ErrorCode::trust_store_io);
}

TEST(TrustStore, UnknownSchemaVersionIsRejected) {
    test::TempStoreDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    {
        std::ofstream out{dir.file()};
        out << "{\"schema_version\":99,\"next_phone_id\":1,\"phones\":[]}";
    }
    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
}

TEST(TrustStore, OverlyWeakIdentityIsRejectedNotApproved) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const trust::WiredIdentity empty{};
    const auto rejected = store.approve(trust::TransportIdentity{empty});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_TRUE(store.phones().empty());
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path};
    out << text;
    (void)::chmod(path.c_str(), 0600);
}

[[nodiscard]] std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

TEST(TrustStore, AllocationAtTheCeilingRoundTrips) {
    test::TempStoreDir dir;
    write_text(dir.file(),
               "{\"schema_version\":2,\"next_phone_id\":4096,\"phones\":["
               "{\"phone_id\":4095,\"identity\":{\"kind\":\"opaque\",\"key\":\"k\"}}]}");
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(approved.has_value());
    EXPECT_EQ(approved.value(), aa::core::PhoneId{4096});

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
}

TEST(TrustStore, AllocationPastTheCeilingFailsClosed) {
    test::TempStoreDir dir;
    write_text(dir.file(), "{\"schema_version\":2,\"next_phone_id\":4097,\"phones\":[]}");
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    const auto before = read_bytes(dir.file());

    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_FALSE(approved.has_value());
    EXPECT_EQ(approved.error().code(), ErrorCode::trust_store_io);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_EQ(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_TRUE(fresh.phones().empty());
}

TEST(TrustStore, BoundaryFieldRoundTrips) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    trust::WiredIdentity boundary{};
    boundary.usb_vendor_id = "18d1";
    boundary.usb_product_id = "4ee1";
    boundary.usb_serial = std::string(trust::kMaxIdentityFieldBytes, 's');
    boundary.port_context = "1-2.3";

    const auto approved = store.approve(trust::TransportIdentity{boundary});
    ASSERT_TRUE(approved.has_value());

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(test::identity_of(trust::TransportIdentity{boundary})),
              trust::Decision::approved);
}

TEST(TrustStore, OverBudgetFieldIsRejectedWithStoreUnchanged) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
    const auto before = read_bytes(dir.file());

    trust::WiredIdentity oversized{};
    oversized.usb_vendor_id = "18d1";
    oversized.usb_product_id = "4ee1";
    oversized.usb_serial = std::string(trust::kMaxIdentityFieldBytes + 1, 's');
    oversized.port_context = "1-2.3";

    const auto rejected = store.approve(trust::TransportIdentity{oversized});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);
}

TEST(TrustStore, OverBudgetDocumentIsRejectedWithStoreUnchanged) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    std::size_t approved = 0;
    std::string before;
    while (true) {
        before = read_bytes(dir.file());
        trust::WiredIdentity bulky{};
        bulky.usb_vendor_id = "18d1";
        bulky.usb_product_id = "4ee1";
        bulky.usb_serial = "s" + std::to_string(approved) + std::string(500, 'x');
        bulky.aoa_manufacturer = std::string(trust::kMaxIdentityFieldBytes, 'm');
        bulky.aoa_model = std::string(trust::kMaxIdentityFieldBytes, 'o');
        bulky.aoa_serial = std::string(trust::kMaxIdentityFieldBytes, 'a');
        bulky.port_context = std::string(trust::kMaxIdentityFieldBytes, 'p');
        const auto saved = store.approve(trust::TransportIdentity{bulky});
        if (!saved.has_value()) {
            EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
            break;
        }
        ++approved;
        ASSERT_LT(approved, 200U);
    }
    EXPECT_GT(approved, 0U);
    EXPECT_EQ(store.phones().size(), approved);
    EXPECT_EQ(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.phones().size(), approved);
}

TEST(TrustStore, DuplicateKeyInputCannotKeepAnIdentityApprovedAfterForget) {
    test::TempStoreDir dir;
    write_text(dir.file(),
               "{\"schema_version\":2,\"next_phone_id\":3,\"phones\":["
               "{\"phone_id\":1,\"identity\":{\"kind\":\"opaque\",\"key\":\"dup-key\"}},"
               "{\"phone_id\":2,\"identity\":{\"kind\":\"opaque\",\"key\":\"dup-key\"}}]}");
    const trust::PhoneIdentity duplicated{"dup-key"};

    trust::ApprovedPhoneStore store{dir.file()};
    const auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
    EXPECT_EQ(store.lookup(duplicated), trust::Decision::unknown);

    const auto forgotten = store.forget_phone(aa::core::PhoneId{1});
    ASSERT_FALSE(forgotten.has_value());

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_FALSE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(duplicated), trust::Decision::unknown);
}

TEST(TrustStore, ApproveOnceBoundaryKeyRoundTrips) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const trust::PhoneIdentity boundary{std::string(trust::kMaxIdentityFieldBytes, 'k')};
    ASSERT_TRUE(store.approve_once(boundary).has_value());

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(boundary), trust::Decision::approved);
}

TEST(TrustStore, ApproveOnceOverBudgetKeyIsRejectedWithStateUnchanged) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
    const auto before = read_bytes(dir.file());

    const trust::PhoneIdentity oversized{std::string(trust::kMaxIdentityFieldBytes + 1, 'k')};
    const auto rejected = store.approve_once(oversized);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(oversized), trust::Decision::unknown);
    EXPECT_EQ(fresh.phones().size(), 1U);
}

TEST(TrustStore, ApproveOnceNonprintableKeyIsRejected) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const trust::PhoneIdentity poisoned{"bad\nkey"};
    const auto rejected = store.approve_once(poisoned);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
}

TEST(TrustStore, ApproveStructuredNonprintableFieldIsRejected) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    trust::WiredIdentity poisoned{};
    poisoned.usb_vendor_id = "18d1";
    poisoned.usb_product_id = "4ee1";
    poisoned.usb_serial = "SER\nIAL";
    poisoned.port_context = "1-2.3";
    const auto rejected = store.approve(trust::TransportIdentity{poisoned});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
}

} // namespace
