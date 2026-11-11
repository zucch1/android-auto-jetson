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

// Independent validation (2026-11-06): Identity.hpp advertises "the STRONGEST
// available transport identity per phone", but validated_key() accepts a
// WiredIdentity that carries NO serial at all (wired_has_material() is happy
// with VID/PID + port_context alone). A phone approved in that state therefore
// authorises EVERY other serial-less device presenting the same VID/PID on the
// same port -- approving attacker phone A silently approves phone B. The
// documented wired trust boundary is physical-port access, but it does not say
// "any device, forever"; a port-only approval should be visibly weaker (or
// refused) rather than recorded as an ordinary identity.
TEST(TrustStore, SerialLessApprovalDoesNotAliasADifferentDevice) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    trust::WiredIdentity phone_a;
    phone_a.usb_vendor_id = "18d1";
    phone_a.usb_product_id = "4ee1";
    phone_a.port_context = "1-2.3";

    trust::WiredIdentity phone_b;
    phone_b.usb_vendor_id = "18d1";
    phone_b.usb_product_id = "4ee1";
    phone_b.port_context = "1-2.3";

    // Control: the two records are structurally identical serial-less devices.
    ASSERT_EQ(trust::identity_key(trust::TransportIdentity{phone_a}),
              trust::identity_key(trust::TransportIdentity{phone_b}));

    const auto implicit = store.approve(trust::TransportIdentity{phone_a});
    ASSERT_FALSE(implicit.has_value());
    EXPECT_EQ(implicit.error().code(), aa::ErrorCode::trust_port_only_requires_opt_in);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{phone_b})),
              trust::Decision::unknown);
    EXPECT_FALSE(store.approve_once(test::identity_of(trust::TransportIdentity{phone_a})).has_value());

    const auto explicit_grant = store.approve_port_only(phone_a);
    ASSERT_TRUE(explicit_grant.has_value());
    const auto repeated = store.approve_port_only(phone_a);
    ASSERT_TRUE(repeated.has_value());
    EXPECT_EQ(repeated.value(), explicit_grant.value());
    ASSERT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(store.phones()[0].strength, trust::IdentityStrength::port_only);
    EXPECT_NE(store.redacted_summary().find("[port-only]"), std::string::npos);
    EXPECT_EQ(store.redacted_summary().find(phone_a.port_context), std::string::npos);
    // Consent is to the port/descriptor tuple; it cannot distinguish these devices.
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{phone_b})),
              trust::Decision::approved);
    EXPECT_FALSE(store.approve(trust::TransportIdentity{phone_a}).has_value());

    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    ASSERT_EQ(fresh.phones().size(), 1U);
    EXPECT_EQ(fresh.phones()[0].strength, trust::IdentityStrength::port_only);
    EXPECT_NE(fresh.redacted_summary().find("[port-only]"), std::string::npos);
    ASSERT_TRUE(fresh.forget_phone(explicit_grant.value()).has_value());
    EXPECT_EQ(fresh.lookup(test::identity_of(trust::TransportIdentity{phone_b})),
              trust::Decision::unknown);
}

TEST(TrustStore, PortOnlyApiRejectsSerialBearingIdentity) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    for (bool usb_serial : {false, true}) {
        auto identity = test::kWired;
        if (usb_serial) { identity.aoa_serial.clear(); }
        else { identity.usb_serial.clear(); }
        const auto approved = store.approve_port_only(identity);
        ASSERT_FALSE(approved.has_value());
        EXPECT_EQ(approved.error().code(), ErrorCode::invalid_argument);
    }
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
}

TEST(TrustStore, WhitespaceSerialsRequireCompletePortBinding) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    for (const auto& wired : {
             trust::WiredIdentity{"", "", " ", "", "", "", ""},
             trust::WiredIdentity{"", "", "", "", "", "   ", ""},
             trust::WiredIdentity{"", "4ee1", " ", "", "", "", "1-2.3"},
             trust::WiredIdentity{"18d1", "", " ", "", "", "", "1-2.3"},
             trust::WiredIdentity{"18d1", "4ee1", " ", "", "", "", ""}}) {
        const auto approved = store.approve_port_only(wired);
        ASSERT_FALSE(approved.has_value());
        EXPECT_EQ(approved.error().code(), ErrorCode::trust_unknown_phone);
        EXPECT_FALSE(trust::validated_key(trust::TransportIdentity{wired}).has_value());
    }
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));

    const trust::WiredIdentity bound{"18d1", "4ee1", " ", "", "", "", "1-2.3"};
    ASSERT_TRUE(store.approve_port_only(bound).has_value());
    ASSERT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(store.phones()[0].strength, trust::IdentityStrength::port_only);
}

TEST(TrustStore, WhitespaceUsbSerialWithRealAoaSerialIsSerialBacked) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    const trust::WiredIdentity wired{"", "", " ", "", "", "real-aoa-serial", ""};
    EXPECT_EQ(trust::strength_of(trust::TransportIdentity{wired}),
              trust::IdentityStrength::serial_backed);
    ASSERT_TRUE(store.approve(trust::TransportIdentity{wired}).has_value());
    ASSERT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(store.phones()[0].strength, trust::IdentityStrength::serial_backed);
    const auto port_only = store.approve_port_only(wired);
    ASSERT_FALSE(port_only.has_value());
    EXPECT_EQ(port_only.error().code(), ErrorCode::invalid_argument);
}

TEST(TrustStore, WhitespaceOnlySerialsRequirePortOnlyConsentWithoutChangingKey) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    auto wired = test::kWired;
    wired.usb_serial = " ";
    wired.aoa_serial = "   ";
    const trust::TransportIdentity identity{wired};
    const auto key = trust::validated_key(identity);
    ASSERT_TRUE(key.has_value());
    EXPECT_NE(key.value().find("1: "), std::string::npos);
    EXPECT_EQ(trust::strength_of(identity), trust::IdentityStrength::port_only);
    const auto implicit = store.approve(identity);
    ASSERT_FALSE(implicit.has_value());
    EXPECT_EQ(implicit.error().code(), ErrorCode::trust_port_only_requires_opt_in);
    ASSERT_TRUE(store.approve_port_only(wired).has_value());
    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(trust::PhoneIdentity{key.value()}), trust::Decision::approved);
    EXPECT_EQ(fresh.phones()[0].strength, trust::IdentityStrength::port_only);
    wired.usb_serial = " S ";
    EXPECT_EQ(trust::strength_of(trust::TransportIdentity{wired}),
              trust::IdentityStrength::serial_backed);
    wired.usb_serial = "\t\n\r\f\v";
    EXPECT_EQ(trust::strength_of(trust::TransportIdentity{wired}),
              trust::IdentityStrength::port_only);
    EXPECT_FALSE(trust::validated_key(trust::TransportIdentity{wired}).has_value());
}

[[nodiscard]] trust::WiredIdentity serial_less_wired() {
    trust::WiredIdentity wired;
    wired.usb_vendor_id = "18d1";
    wired.usb_product_id = "4ee1";
    wired.aoa_manufacturer = "Google";
    wired.aoa_model = "Pixel";
    wired.port_context = "1-2.3";
    return wired;
}

// Review fix (PR #93, P1): the M12 decision is made on the canonical KEY, not
// the stored variant. A legacy/opaque record carrying a serial-less "w1:" key
// must not authorise another serial-less device presenting the same key.
TEST(TrustStore, OpaqueSerialLessWiredKeyDoesNotAuthoriseAnotherDevice) {
    test::TempStoreDir dir;
    const std::string port_only_key =
        trust::identity_key(trust::TransportIdentity{serial_less_wired()});
    ASSERT_EQ(trust::classify_wired_key(port_only_key), trust::WiredKeyClass::port_only);
    write_text(dir.file(),
               "{\"schema_version\":2,\"next_phone_id\":3,\"phones\":["
               "{\"phone_id\":1,\"identity\":{\"kind\":\"opaque\",\"key\":\"" +
                   port_only_key +
                   "\"}},"
                   "{\"phone_id\":2,\"identity\":{\"kind\":\"opaque\",\"key\":\"w1:4:18d1:9\"}}]}");

    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    EXPECT_TRUE(store.phones().empty());
    EXPECT_EQ(store.dropped_port_only_count(), 2U);

    // Another serial-less device of the same model on the same port presents
    // exactly the stored key.
    const auto other_device = test::identity_of(trust::TransportIdentity{serial_less_wired()});
    ASSERT_EQ(other_device.key, port_only_key);
    EXPECT_EQ(store.lookup(other_device), trust::Decision::unknown);
    EXPECT_FALSE(trust::allows_session(store.lookup(other_device)));
    EXPECT_EQ(store.lookup(trust::PhoneIdentity{"w1:4:18d1:9"}), trust::Decision::unknown)
        << "a malformed w1 key must fail closed";

    EXPECT_TRUE(store.redacted_summary().empty());

    // Re-approving the same key is refused, not answered with the legacy id.
    const auto reapproved = store.approve(trust::TransportIdentity{serial_less_wired()});
    ASSERT_FALSE(reapproved.has_value());
    EXPECT_EQ(reapproved.error().code(), ErrorCode::trust_port_only_requires_opt_in);
}

TEST(TrustStore, ApproveOnceSerialLessWiredKeyIsRejected) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    const auto port_only = test::identity_of(trust::TransportIdentity{serial_less_wired()});
    const auto rejected = store.approve_once(port_only);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::trust_unknown_phone);

    const auto malformed = store.approve_once(trust::PhoneIdentity{"w1:not-a-canonical-key"});
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(malformed.error().code(), ErrorCode::trust_unknown_phone);

    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file())) << "a refused approval persists nothing";
    EXPECT_EQ(store.lookup(port_only), trust::Decision::unknown);

    // All opaque wired approvals require the structured seam, including serial-backed.
    const auto strong = test::identity_of(trust::TransportIdentity{test::kWired});
    EXPECT_FALSE(store.approve_once(strong).has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
    EXPECT_EQ(store.lookup(strong), trust::Decision::approved);
}

TEST(TrustStore, WiredKeyClassificationFailsClosed) {
    using trust::WiredKeyClass;
    EXPECT_EQ(trust::classify_wired_key(trust::identity_key(trust::TransportIdentity{test::kWired})),
              WiredKeyClass::serial_bound);
    EXPECT_EQ(trust::classify_wired_key(
                  trust::identity_key(trust::TransportIdentity{serial_less_wired()})),
              WiredKeyClass::port_only);
    trust::WiredIdentity usb_only = serial_less_wired();
    usb_only.usb_serial = "S";
    EXPECT_EQ(trust::classify_wired_key(trust::identity_key(trust::TransportIdentity{usb_only})),
              WiredKeyClass::serial_bound);
    trust::WiredIdentity aoa_only = serial_less_wired();
    aoa_only.aoa_serial = "A";
    EXPECT_EQ(trust::classify_wired_key(trust::identity_key(trust::TransportIdentity{aoa_only})),
              WiredKeyClass::serial_bound);
    // A field containing ':' and digits is still parsed by its length prefix.
    trust::WiredIdentity tricky = serial_less_wired();
    tricky.aoa_model = "0:0:0:";
    EXPECT_EQ(trust::classify_wired_key(trust::identity_key(trust::TransportIdentity{tricky})),
              WiredKeyClass::port_only);

    EXPECT_EQ(trust::classify_wired_key(trust::identity_key(trust::TransportIdentity{test::kWireless})),
              WiredKeyClass::not_wired);
    EXPECT_EQ(trust::classify_wired_key("legacy-key"), WiredKeyClass::not_wired);

    const std::string canonical = trust::identity_key(trust::TransportIdentity{serial_less_wired()});
    for (const std::string& bad : {std::string{"w1:"}, canonical + "x", canonical + "0:",
                                   canonical.substr(0, canonical.size() - 1),
                                   std::string{"w1:04:18d14:4ee10:0:6:Google5:Pixel0:5:1-2.3"},
                                   std::string{"w1:4:18d14:4ee10:6:Google5:Pixel5:1-2.3"},
                                   std::string{"w1:99999999999999999999999:x"},
                                   std::string{"w1:x:"}}) {
        EXPECT_EQ(trust::classify_wired_key(bad), WiredKeyClass::malformed) << bad;
    }
}

} // namespace
