// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 replacement durability (fault-injected via linker-wrapped
// fsync/renameat): every failure BEFORE the rename rolls the mutation back
// with the store unchanged in memory and on disk; a directory-sync failure
// AFTER the real rename is the committed-but-not-durable outcome
// (trust_store_not_durable) and the mutation is kept in memory, on disk and
// in the next phone-id sequence.
#include <aa/trust/Store.hpp>

#include "fakes.hpp"
#include "syscall_faults.hpp"

#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
namespace faults = aa::trust::test::faults;
using aa::ErrorCode;

[[nodiscard]] std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void establish_baseline(trust::ApprovedPhoneStore& store) {
    EXPECT_TRUE(store.load().has_value());
    const auto first = store.approve(trust::TransportIdentity{test::kWired});
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), aa::core::PhoneId{1});
}

TEST(TrustStore, TempFileFsyncFailureRollsBackTheApproval) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);
    const std::string before = read_bytes(dir.file());

    {
        const faults::ScopedFault fault{faults::Fault::temp_fsync};
        const auto saved = store.approve(trust::TransportIdentity{test::kWireless});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.phones().size(), 1U);
    EXPECT_EQ(reloaded.lookup(test::identity_of(trust::TransportIdentity{test::kWireless})),
              trust::Decision::unknown);
    const auto retry = reloaded.approve(trust::TransportIdentity{test::kWireless});
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry.value(), aa::core::PhoneId{2});
}

TEST(TrustStore, RenameFailureRollsBackTheApproval) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);
    const std::string before = read_bytes(dir.file());

    {
        const faults::ScopedFault fault{faults::Fault::renameat};
        const auto saved = store.approve(trust::TransportIdentity{test::kWireless});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.phones().size(), 1U);
    const auto retry = reloaded.approve(trust::TransportIdentity{test::kWireless});
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry.value(), aa::core::PhoneId{2});
}

TEST(TrustStore, DirFsyncFailureAfterRenameKeepsMutationAndReportsNotDurable) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);
    const std::string before = read_bytes(dir.file());

    {
        const faults::ScopedFault fault{faults::Fault::dir_fsync};
        const auto saved = store.approve(trust::TransportIdentity{test::kWireless});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_not_durable);
        EXPECT_EQ(saved.error().message(), "Trust store replaced; crash durability unconfirmed.");
    }

    EXPECT_EQ(store.phones().size(), 2U);
    EXPECT_EQ(store.phones()[1].phone_id, aa::core::PhoneId{2});
    EXPECT_NE(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.phones().size(), 2U);
    EXPECT_EQ(reloaded.lookup(test::identity_of(trust::TransportIdentity{test::kWireless})),
              trust::Decision::approved);
    const auto next = reloaded.approve(
        trust::TransportIdentity{trust::WiredIdentity{"18d1", "4ee1", "OTHER", "", "", "", "1-2.3"}});
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value(), aa::core::PhoneId{3});
}

TEST(TrustStore, DirFsyncEINTRIsRetriedAndSucceeds) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);

    {
        const faults::ScopedFault fault{faults::Fault::dir_fsync_eintr_once};
        const auto saved = store.approve(trust::TransportIdentity{test::kWireless});
        ASSERT_TRUE(saved.has_value());
        EXPECT_EQ(saved.value(), aa::core::PhoneId{2});
    }

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.phones().size(), 2U);
}

TEST(TrustStore, ApproveOnceTempFsyncFailureRollsBack) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);
    const std::string before = read_bytes(dir.file());

    {
        const faults::ScopedFault fault{faults::Fault::temp_fsync};
        const auto saved = store.approve_once(trust::PhoneIdentity{"legacy-key"});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.lookup(trust::PhoneIdentity{"legacy-key"}), trust::Decision::unknown);
}

TEST(TrustStore, ApproveOnceRenameFailureRollsBack) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);
    const std::string before = read_bytes(dir.file());

    {
        const faults::ScopedFault fault{faults::Fault::renameat};
        const auto saved = store.approve_once(trust::PhoneIdentity{"legacy-key"});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);
}

TEST(TrustStore, ApproveOnceDirFsyncFailureAfterRenameKeepsOpaqueApproval) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);

    {
        const faults::ScopedFault fault{faults::Fault::dir_fsync};
        const auto saved = store.approve_once(trust::PhoneIdentity{"legacy-key"});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_not_durable);
    }

    EXPECT_EQ(store.phones().size(), 2U);
    EXPECT_EQ(store.phones()[1].phone_id, aa::core::PhoneId{2});

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.lookup(trust::PhoneIdentity{"legacy-key"}), trust::Decision::approved);
    ASSERT_TRUE(reloaded.approve_once(trust::PhoneIdentity{"other-key"}).has_value());
    ASSERT_EQ(reloaded.phones().size(), 3U);
    EXPECT_EQ(reloaded.phones()[2].phone_id, aa::core::PhoneId{3});
}

TEST(TrustStore, ForgetPhoneTempFsyncFailureKeepsApproval) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);
    const std::string before = read_bytes(dir.file());

    {
        const faults::ScopedFault fault{faults::Fault::temp_fsync};
        const auto saved = store.forget_phone(aa::core::PhoneId{1});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
}

TEST(TrustStore, ForgetPhoneRenameFailureKeepsApproval) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);
    const std::string before = read_bytes(dir.file());

    {
        const faults::ScopedFault fault{faults::Fault::renameat};
        const auto saved = store.forget_phone(aa::core::PhoneId{1});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_EQ(read_bytes(dir.file()), before);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
}

TEST(TrustStore, ForgetPhoneDirFsyncFailureAfterRenameKeepsRevocation) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    establish_baseline(store);

    {
        const faults::ScopedFault fault{faults::Fault::dir_fsync};
        const auto saved = store.forget_phone(aa::core::PhoneId{1});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_not_durable);
    }

    EXPECT_TRUE(store.phones().empty());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_TRUE(reloaded.phones().empty());
    EXPECT_EQ(reloaded.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
    const auto next = reloaded.approve(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value(), aa::core::PhoneId{2});
}

TEST(TrustStore, AncestorDirFsyncFailureFailsClosedBeforeReplacement) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());

    {
        const faults::ScopedFault fault{faults::Fault::dir_fsync};
        const auto saved = store.approve(trust::TransportIdentity{test::kWired});
        ASSERT_FALSE(saved.has_value());
        EXPECT_EQ(saved.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
}

} // namespace
