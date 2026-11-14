// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/helper/bluez_adapter/ManagedPeers.hpp"
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <gtest/gtest.h>
#include "../trust/syscall_faults.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
namespace {
using namespace aa;
using helper::bluez_adapter::ManagedPeers;
const helper::PeerIdentity managed{"AA:BB:CC:DD:EE:FF", "00:11:22:33:44:55"};
class ManagedBonds : public testing::Test {
protected:
    std::string directory;
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "aa-bonds-XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr); directory = pattern;
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
};
TEST_F(ManagedBonds, RestartKeepsRevocationAuthorityAndRefusesUnmanagedBond) {
    { ManagedPeers before{directory, ::getuid()}; ASSERT_TRUE(before.valid()); ASSERT_TRUE(before.remember(managed)); }
    ManagedPeers after{directory, ::getuid()};
    ASSERT_TRUE(after.valid()); EXPECT_TRUE(after.contains(managed));
    EXPECT_FALSE(after.contains({"11:22:33:44:55:66", managed.adapter}));
    EXPECT_FALSE(after.contains({managed.address, "11:22:33:44:55:66"}));
}
TEST_F(ManagedBonds, CorruptStateFailsClosedWithoutBeingOverwritten) {
    const auto path = directory + "/managed-bonds";
    { std::ofstream out{path}; out << "v1\nAA:BB:CC:DD:EE:FF 00:11:22:33:44:55\ntruncated"; }
    ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    ManagedPeers peers{directory, ::getuid()};
    EXPECT_FALSE(peers.valid()); EXPECT_FALSE(peers.contains(managed)); EXPECT_FALSE(peers.remember(managed));
}
TEST_F(ManagedBonds, TamperedModeAndSymlinkFailClosed) {
    { ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.remember(managed)); }
    const auto path = directory + "/managed-bonds";
    ASSERT_EQ(::chmod(path.c_str(), 0666), 0);
    EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.valid()));
    ASSERT_EQ(::rename(path.c_str(), (path + ".target").c_str()), 0);
    ASSERT_EQ(::symlink("managed-bonds.target", path.c_str()), 0);
    EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.valid()));
}
TEST_F(ManagedBonds, RejectsWrongOwnerHardlinksAndWritableDirectory) {
    { ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.remember(managed)); }
    EXPECT_FALSE((ManagedPeers{directory, ::getuid() + 1}.valid()));
    EXPECT_FALSE((ManagedPeers{directory + "/", ::getuid()}.valid()));
    const auto alias = directory + "-alias";
    ASSERT_EQ(::symlink(directory.c_str(), alias.c_str()), 0);
    EXPECT_FALSE((ManagedPeers{alias, ::getuid()}.valid()));
    ASSERT_EQ(::unlink(alias.c_str()), 0);
    const auto path = directory + "/managed-bonds";
    ASSERT_EQ(::link(path.c_str(), (path + ".link").c_str()), 0);
    EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.valid()));
    ASSERT_EQ(::unlink((path + ".link").c_str()), 0);
    ASSERT_EQ(::chmod(directory.c_str(), 0770), 0);
    EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.valid()));
}
TEST_F(ManagedBonds, StaleStagingFileNeitherGrantsAuthorityNorBlocksAdmission) {
    { std::ofstream out{directory + "/managed-bonds.new"}; out << "foreign"; }
    ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.valid());
    EXPECT_FALSE(peers.contains(managed));
    EXPECT_TRUE(peers.remember(managed)); EXPECT_TRUE(peers.contains(managed));
    EXPECT_TRUE((ManagedPeers{directory, ::getuid()}.contains(managed)));
}
TEST_F(ManagedBonds, StrictFormatRejectsMalformedDuplicateAndTrailingRecords) {
    for (const auto& text : {std::string{"v2\n"}, std::string{"v1\nAA:BB:CC:DD:EE:ff 00:11:22:33:44:55\n"},
            std::string{"v1\nAA:BB:CC:DD:EE:FF 00:11:22:33:44:55\nAA:BB:CC:DD:EE:FF 00:11:22:33:44:55\n"}}) {
        const auto path = directory + "/managed-bonds";
        { std::ofstream out{path}; out << text; }
        ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
        EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.valid()));
    }
}
std::size_t leftovers(const std::string& directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().filename() != "managed-bonds") ++count;
    return count;
}
const helper::PeerIdentity other{"11:22:33:44:55:66", managed.adapter};
const helper::PeerIdentity unmanaged{"99:88:77:66:55:44", managed.adapter};
// A failed write refuses only NEW admissions: revocation authority for bonds
// already managed (the audit's M4) must survive every persistence failure.
TEST_F(ManagedBonds, FailedWriteRefusesNewAdmissionButKeepsRevocationAuthority) {
    using namespace trust::test::faults;
    for (const auto fault : {Fault::temp_fsync, Fault::renameat}) {
        ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.valid());
        ASSERT_TRUE(peers.remember(managed));
        { ScopedFault fail{fault}; EXPECT_FALSE(peers.remember(other)); }
        EXPECT_TRUE(peers.valid());
        EXPECT_TRUE(peers.contains(managed)) << "revocation of a known bond was lost";
        EXPECT_FALSE(peers.contains(unmanaged));
        EXPECT_FALSE(peers.contains(other)) << "a failed commit admitted the peer";
        EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.contains(other)));
        EXPECT_EQ(leftovers(directory), 0u) << "staging file not removed";
        EXPECT_TRUE(peers.remember(other)) << "a transient failure must not latch";
        EXPECT_TRUE(peers.contains(managed)); EXPECT_TRUE(peers.contains(other));
        std::filesystem::remove(directory + "/managed-bonds");
    }
}
TEST_F(ManagedBonds, RevalidationFailureRefusesAdmissionOnlyAndRetriesEintr) {
    ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.remember(managed));
    ASSERT_EQ(::chmod(directory.c_str(), 0500), 0);  // directory no longer private: cannot write
    EXPECT_FALSE(peers.remember(other));
    EXPECT_TRUE(peers.contains(managed)); EXPECT_FALSE(peers.contains(other)); EXPECT_FALSE(peers.contains(unmanaged));
    ASSERT_EQ(::chmod(directory.c_str(), 0700), 0);
    EXPECT_TRUE(peers.remember(other));
    using namespace trust::test::faults;
    { ScopedFault interrupted{Fault::dir_fsync_eintr_once};
      EXPECT_TRUE(peers.remember(unmanaged)) << "EINTR on the directory fsync must be retried"; }
}
// Past the rename the entry is on disk: it counts for revocation even when
// the directory fsync fails, but that admission is refused and not repeated
// until durability is confirmed.
TEST_F(ManagedBonds, UncertainDirectorySyncKeepsEntryRevocableAndRefusesAdmission) {
    using namespace trust::test::faults;
    ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.remember(other));
    { ScopedFault fail{Fault::dir_fsync}; EXPECT_FALSE(peers.remember(managed));
      EXPECT_TRUE(peers.valid()); EXPECT_TRUE(peers.contains(managed)); EXPECT_TRUE(peers.contains(other));
      EXPECT_FALSE(peers.contains(unmanaged));
      EXPECT_FALSE(peers.remember(managed)) << "re-admission before durability is confirmed";
      EXPECT_FALSE(peers.remember(unmanaged)); }
    EXPECT_FALSE(peers.contains(unmanaged));
    EXPECT_TRUE(peers.remember(managed)) << "admission resumes once the directory syncs";
    EXPECT_TRUE(peers.remember(unmanaged));
    ManagedPeers loaded{directory, ::getuid()}; ASSERT_TRUE(loaded.valid());
    EXPECT_TRUE(loaded.contains(managed)); EXPECT_TRUE(loaded.contains(other)); EXPECT_TRUE(loaded.contains(unmanaged));
}
// P3b: the unaccepted-bond removal list shares the file rules but is never revocation authority.
TEST_F(ManagedBonds, PendingRemovalListPersistsForgetsAndIsNotRevocationAuthority) {
    using namespace trust::test::faults;
    const auto reopen = [&] { return ManagedPeers{directory, ::getuid(), "pending-removals"}; };
    { auto pending = reopen(); ASSERT_TRUE(pending.valid()); ASSERT_TRUE(pending.remember(managed)); ASSERT_TRUE(pending.remember(other)); }
    auto pending = reopen();
    ASSERT_TRUE(pending.valid()); EXPECT_EQ(pending.peers().size(), 2u) << "a restart lost a pending removal";
    EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.contains(managed))) << "pending removal became revocation authority";
    { ScopedFault fail{Fault::renameat}; EXPECT_FALSE(pending.forget(managed)); }
    EXPECT_TRUE(pending.contains(managed)) << "a failed rewrite dropped the entry";
    EXPECT_TRUE(reopen().contains(managed));
    ASSERT_TRUE(pending.forget(managed)); EXPECT_FALSE(pending.contains(managed)); EXPECT_TRUE(pending.contains(other));
    EXPECT_TRUE(pending.forget(unmanaged)) << "forgetting an absent entry is a no-op";
    ASSERT_TRUE(pending.forget(other));
    const auto emptied = reopen();
    EXPECT_TRUE(emptied.valid()); EXPECT_TRUE(emptied.peers().empty());
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        EXPECT_NE(entry.path().filename().string().front(), '.') << "staging file left behind";
    const auto path = directory + "/pending-removals";
    ASSERT_EQ(::chmod(path.c_str(), 0666), 0);
    EXPECT_FALSE(reopen().valid()) << "the same mode rules apply to the pending list";
}
// Journal durability: forget() is true only when the removal is durable, and the unconfirmed
// directory sync is retried by the next forget().
TEST_F(ManagedBonds, PendingRemovalForgetReportsSuccessOnlyOnceDurable) {
    using namespace trust::test::faults;
    ManagedPeers pending{directory, ::getuid(), "pending-removals"};
    ASSERT_TRUE(pending.remember(managed)); ASSERT_TRUE(pending.remember(other));
    { ScopedFault fail{Fault::dir_fsync};
      EXPECT_FALSE(pending.forget(managed)) << "success reported although the directory fsync failed";
      EXPECT_FALSE(pending.contains(managed)) << "renamed into place: gone from the in-memory list";
      EXPECT_FALSE(pending.forget(managed)) << "still not durable while the directory fsync keeps failing"; }
    EXPECT_TRUE(pending.forget(managed)) << "the next forget confirms the directory sync";
    EXPECT_TRUE(pending.forget(managed));
    EXPECT_TRUE(pending.contains(other));
    { ScopedFault fail{Fault::renameat}; EXPECT_FALSE(pending.forget(other)); }
    EXPECT_TRUE(pending.contains(other));
    EXPECT_TRUE(pending.forget(other));
}
TEST_F(ManagedBonds, OversizedStateFileFailsClosedWithReason) {
    const auto path = directory + "/managed-bonds";
    { std::ofstream out{path}; out << "v1\n" << std::string(64 * 1024, 'A'); }
    ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    ManagedPeers peers{directory, ::getuid()};
    EXPECT_FALSE(peers.valid()); EXPECT_FALSE(peers.contains(managed)); EXPECT_FALSE(peers.remember(managed));
    EXPECT_FALSE(peers.reason().empty());
}
TEST_F(ManagedBonds, EntryLimitRefusesNewAdmissionButKeepsRevocation) {
    std::string text{"v1\n"};
    for (int i = 0; i < 256; ++i) {
        char line[40]; std::snprintf(line, sizeof line, "02:00:00:00:%02X:%02X %s\n", i / 16, i % 16, managed.adapter.c_str());
        text += line;
    }
    const auto path = directory + "/managed-bonds";
    { std::ofstream out{path}; out << text; }
    ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.valid());
    EXPECT_TRUE(peers.contains({"02:00:00:00:0F:0F", managed.adapter}));
    EXPECT_FALSE(peers.remember(managed));
    { std::ofstream out{path, std::ios::app}; out << "02:00:00:00:10:00 " << managed.adapter << "\n"; }
    EXPECT_FALSE((ManagedPeers{directory, ::getuid()}.valid())) << "257 records";
}
TEST_F(ManagedBonds, SymlinkInMiddleDirectoryOfStatePathFailsClosed) {
    ASSERT_EQ(::mkdir((directory + "/real").c_str(), 0700), 0);
    ASSERT_EQ(::mkdir((directory + "/real/state").c_str(), 0700), 0);
    { ManagedPeers peers{directory + "/real/state", ::getuid()}; ASSERT_TRUE(peers.valid()); ASSERT_TRUE(peers.remember(managed)); }
    ASSERT_EQ(::symlink((directory + "/real").c_str(), (directory + "/mid").c_str()), 0);
    ManagedPeers through{directory + "/mid/state", ::getuid()};
    EXPECT_FALSE(through.valid()); EXPECT_FALSE(through.contains(managed)); EXPECT_FALSE(through.remember(other));
    EXPECT_FALSE(through.reason().empty());
    EXPECT_TRUE((ManagedPeers{directory + "/real/state", ::getuid()}.contains(managed)));
}
TEST(HelperOwner, NonRootOwnerIsRefusedBeforeAnyHelperCall) {
    if (::getuid() == 0) GTEST_SKIP() << "requires non-root bus owner";
    using namespace ipc::dbus_adapter;
    auto service = BusConnection::connect_session(); ASSERT_TRUE(service);
    ASSERT_TRUE(service.value().request_name(ipc::BusName{helper::kBusName}));
    auto pattern = (std::filesystem::temp_directory_path() / "aa-owner-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    trust::ApprovedPhoneStore store{std::filesystem::path{pattern} / "phones"}; ASSERT_TRUE(store.load());
    auto client = helper::BusClient::private_bus(std::getenv("DBUS_SESSION_BUS_ADDRESS"), store); ASSERT_TRUE(client);
    const auto result = client.value()->status(helper::Action::ap_status);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::helper_rejected);
    std::filesystem::remove_all(pattern);
}
}
