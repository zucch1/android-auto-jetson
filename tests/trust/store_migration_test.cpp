// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 store migration: schema_version 1 (legacy key-only records) loads by
// migrating to opaque identities and persists as version 3; malformed legacy
// records are rejected and never silently trusted.
#include <aa/trust/Store.hpp>

#include "fakes.hpp"
#include "syscall_faults.hpp"
#include "../../src/trust/StoreSchema.hpp"

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
    EXPECT_EQ(store.dropped_port_only_count(), 1U);
    const auto approved = store.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(approved.has_value());
    EXPECT_EQ(approved.value(), aa::core::PhoneId{8});
    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.dropped_port_only_count(), 0U);
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
        EXPECT_EQ(store.dropped_port_only_count(), 1U);
        EXPECT_EQ(store.lookup(trust::PhoneIdentity{key}), trust::Decision::unknown);
    }
}

TEST(StoreMigration, V3RejectsOpaquePortOnlyWiredKeys) {
    for (const std::string serial : {"", "   "}) {
        test::TempStoreDir dir;
        const trust::TransportIdentity weak{trust::WiredIdentity{
            "18d1", "4ee1", serial, "", "", "", "1-2.3"}};
        const auto key = trust::identity_key(weak);
        write_text(dir.file(),
            "{\"schema_version\":3,\"next_phone_id\":2,\"phones\":[{\"phone_id\":1,"
            "\"strength\":\"opaque\",\"identity\":{\"kind\":\"opaque\",\"key\":\"" + key + "\"}}]}");
        trust::ApprovedPhoneStore store{dir.file()};
        const auto loaded = store.load();
        ASSERT_FALSE(loaded.has_value());
        EXPECT_EQ(loaded.error().code(), ErrorCode::trust_store_io);
        EXPECT_FALSE(store.usable());
        EXPECT_EQ(store.lookup(trust::PhoneIdentity{key}), trust::Decision::unknown);
        EXPECT_TRUE(store.phones().empty());
        EXPECT_EQ(store.dropped_port_only_count(), 0U);
    }
}

TEST(StoreMigration, V3OpaqueSerialBackedWiredKeyRoundTrips) {
    test::TempStoreDir dir;
    const auto key = trust::identity_key(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(key.starts_with("w1:"));
    ASSERT_EQ(trust::strength_of(trust::TransportIdentity{test::kWired}),
              trust::IdentityStrength::serial_backed);
    write_text(dir.file(),
        "{\"schema_version\":3,\"next_phone_id\":8,\"phones\":[{\"phone_id\":7,"
        "\"strength\":\"opaque\",\"identity\":{\"kind\":\"opaque\",\"key\":\"" + key + "\"}}]}");
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    EXPECT_EQ(store.lookup(trust::PhoneIdentity{key}), trust::Decision::approved);
    ASSERT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(store.phones()[0].strength, trust::IdentityStrength::opaque);
    EXPECT_EQ(store.dropped_port_only_count(), 0U);

    // A new approval persists the loaded compatibility record unchanged.
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWireless}).has_value());
    trust::ApprovedPhoneStore fresh{dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_EQ(fresh.lookup(trust::PhoneIdentity{key}), trust::Decision::approved);
    ASSERT_EQ(fresh.phones().size(), 2U);
    EXPECT_EQ(fresh.phones()[0].phone_id, aa::core::PhoneId{7});
    EXPECT_EQ(fresh.phones()[0].strength, trust::IdentityStrength::opaque);
    EXPECT_EQ(trust::identity_key(fresh.phones()[0].identity), key);
}

TEST(StoreMigration, WriterDerivesStrengthFromIdentity) {
    trust::detail::ParsedStore snapshot;
    snapshot.phones = {
        {aa::core::PhoneId{1}, trust::TransportIdentity{test::kWired}},
        {aa::core::PhoneId{2}, trust::TransportIdentity{test::kWireless}},
        {aa::core::PhoneId{3}, trust::TransportIdentity{trust::WiredIdentity{
            "18d1", "4ee1", "", "", "", "", "1-2.3"}}}};
    snapshot.next_phone_id = 4;
    const auto parsed = trust::detail::parse_store(trust::detail::write_store(snapshot));
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed.value().phones.size(), 3U);
    EXPECT_EQ(parsed.value().phones[0].strength, trust::IdentityStrength::serial_backed);
    EXPECT_EQ(parsed.value().phones[1].strength, trust::IdentityStrength::bond_backed);
    EXPECT_EQ(parsed.value().phones[2].strength, trust::IdentityStrength::port_only);
}

std::string recovery_document(const std::vector<std::string>& addresses) {
    trust::detail::ParsedStore snapshot;
    snapshot.phones.push_back({aa::core::PhoneId{1}, test::kWired});
    for (const auto& address : addresses)
        snapshot.phones.push_back({aa::core::PhoneId{snapshot.phones.size() + 1},
                                  trust::WirelessIdentity{address, "hci0"}});
    snapshot.next_phone_id = snapshot.phones.size() + 1;
    return trust::detail::write_store(snapshot);
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(StoreMigration, QuarantinesLegacyNonMacWithPrivateOneTimeBackupAndDiagnostic) {
    for (int version : {2, 3}) {
        test::TempStoreDir dir;
        auto original = recovery_document({"legacy-bond", "AA:BB:CC:DD:EE:FF"});
        if (version == 2) {
            original.replace(original.find("schema_version\":3"), 17, "schema_version\":2");
            // V2 has no strength member (including the wired survivor).
            for (const std::string tier : {"serial-backed", "bond-backed"}) {
                const auto field = ",\"strength\":\"" + tier + "\"";
                for (auto at = original.find(field); at != std::string::npos; at = original.find(field))
                    original.erase(at, field.size());
            }
        }
        write_text(dir.file(), original);
        trust::ApprovedPhoneStore store{dir.file()};
        testing::internal::CaptureStderr();
        const auto loaded = store.load();
        const auto diagnostic = testing::internal::GetCapturedStderr();
        ASSERT_TRUE(loaded);
        EXPECT_NE(diagnostic.find("1 invalid legacy wireless address records"), std::string::npos);
        ASSERT_EQ(store.phones().size(), 2U);
        EXPECT_EQ(store.phones()[0].phone_id, aa::core::PhoneId{1});
        EXPECT_EQ(store.phones()[1].phone_id, aa::core::PhoneId{3});
        const auto backup = std::filesystem::path{dir.file().string() + ".legacy-backup"};
        EXPECT_EQ(read_text(backup), original);
        struct stat info {};
        ASSERT_EQ(::stat(backup.c_str(), &info), 0);
        EXPECT_EQ(info.st_mode & 0777, 0600U);
        EXPECT_EQ(info.st_uid, ::geteuid());
        trust::ApprovedPhoneStore fresh{dir.file()};
        ASSERT_TRUE(fresh.load());
        EXPECT_EQ(fresh.phones().size(), 2U);
        // A later legacy import cannot overwrite the first original.
        write_text(dir.file(), recovery_document({"another-legacy-bond"}));
        ASSERT_TRUE(fresh.load());
        EXPECT_EQ(read_text(backup), original);
    }
}

TEST(StoreMigration, QuarantinesEveryAddressCaseCollisionWithoutMerging) {
    test::TempStoreDir dir;
    const auto original = recovery_document({"aa:bb:cc:dd:ee:ff", "AA:BB:CC:DD:EE:FF", "Aa:Bb:Cc:Dd:Ee:Ff"});
    write_text(dir.file(), original);
    trust::ApprovedPhoneStore store{dir.file()};
    testing::internal::CaptureStderr();
    const auto loaded = store.load();
    const auto diagnostic = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(loaded);
    ASSERT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(store.lookup(test::identity_of(test::kWired)), trust::Decision::approved);
    EXPECT_EQ(store.lookup(test::identity_of(test::kWireless)), trust::Decision::unknown);
    EXPECT_NE(diagnostic.find("3 address-case collision records"), std::string::npos);
    EXPECT_EQ(read_text(dir.file().string() + ".legacy-backup"), original);
    const auto added = store.approve(test::kWireless);
    ASSERT_TRUE(added);
    EXPECT_EQ(added.value(), aa::core::PhoneId{5});
}

TEST(StoreMigration, RecoveryDoesNotHideCorruptionOrFollowBackupSymlinks) {
    test::TempStoreDir dir;
    const auto original = recovery_document({"legacy-bond"});
    for (const auto& text : {original + "garbage", std::string{"{bad json"},
             recovery_document({"AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FF"})}) {
        write_text(dir.file(), text);
        trust::ApprovedPhoneStore store{dir.file()};
        EXPECT_FALSE(store.load());
        EXPECT_FALSE(store.usable());
        EXPECT_FALSE(std::filesystem::exists(dir.file().string() + ".legacy-backup"));
        EXPECT_EQ(read_text(dir.file()), text);
    }
    for (const std::string corruption : {"version", "counter", "strength", "adapter", "phone-id"}) {
        auto text = original;
        if (corruption == "version") text.replace(text.find("schema_version\":3"), 17, "schema_version\":9");
        if (corruption == "counter") text.replace(text.find("next_phone_id\":3"), 16, "next_phone_id\":2");
        if (corruption == "strength") text.replace(text.find("bond-backed"), 11, "opaque");
        if (corruption == "adapter") text.replace(text.find("hci0"), 4, "bad\\nadapter");
        if (corruption == "phone-id") text.replace(text.find("phone_id\":2"), 11, "phone_id\":1");
        write_text(dir.file(), text);
        trust::ApprovedPhoneStore store{dir.file()};
        EXPECT_FALSE(store.load()) << corruption;
        EXPECT_FALSE(store.usable());
        EXPECT_FALSE(std::filesystem::exists(dir.file().string() + ".legacy-backup"));
        EXPECT_EQ(read_text(dir.file()), text);
    }
    write_text(dir.file(), original);
    const auto target = dir.dir() / "untouched";
    write_text(target, "sentinel");
    std::filesystem::create_symlink(target, dir.file().string() + ".legacy-backup");
    trust::ApprovedPhoneStore store{dir.file()};
    EXPECT_FALSE(store.load());
    EXPECT_FALSE(store.usable());
    EXPECT_EQ(read_text(target), "sentinel");
    EXPECT_EQ(read_text(dir.file()), original);
}

TEST(StoreMigration, RecoveryWriteFailureKeepsOriginalUntrustedUntilRetry) {
    test::TempStoreDir dir;
    const auto original = recovery_document({"legacy-bond", "lEgACy-BonD"});
    write_text(dir.file(), original);
    trust::ApprovedPhoneStore store{dir.file()};
    {
        const test::faults::ScopedFault fault{test::faults::Fault::renameat};
        EXPECT_FALSE(store.load());
        EXPECT_FALSE(store.usable());
        EXPECT_TRUE(store.phones().empty());
    }
    EXPECT_EQ(read_text(dir.file()), original);
    EXPECT_EQ(read_text(dir.file().string() + ".legacy-backup"), original);
    ASSERT_TRUE(store.load());
    ASSERT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(store.lookup(test::identity_of(test::kWired)), trust::Decision::approved);
}

} // namespace
