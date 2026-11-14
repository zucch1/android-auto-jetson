// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/helper/bluez_adapter/ManagedPeers.hpp"
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <aa/helper/BluezBackend.hpp>
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <gtest/gtest.h>
#include "../trust/syscall_faults.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
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
    { ManagedPeers before{directory, ::getuid()}; ASSERT_TRUE(before.valid()); ASSERT_TRUE(before.remember(managed));
      EXPECT_TRUE(before.revocable(managed)); EXPECT_TRUE(before.admitted(managed)); }
    ManagedPeers after{directory, ::getuid()};
    ASSERT_TRUE(after.valid()); EXPECT_TRUE(after.revocable(managed)); EXPECT_TRUE(after.admitted(managed));
    EXPECT_FALSE(after.revocable({"11:22:33:44:55:66", managed.adapter}));
    EXPECT_FALSE(after.admitted({"11:22:33:44:55:66", managed.adapter}));
    EXPECT_FALSE(after.revocable({managed.address, "11:22:33:44:55:66"}));
    EXPECT_FALSE(after.admitted({managed.address, "11:22:33:44:55:66"}));
}
TEST_F(ManagedBonds, CorruptStateFailsClosedWithoutBeingOverwritten) {
    const auto path = directory + "/managed-bonds";
    { std::ofstream out{path}; out << "v1\nAA:BB:CC:DD:EE:FF 00:11:22:33:44:55\ntruncated"; }
    ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    ManagedPeers peers{directory, ::getuid()};
    EXPECT_FALSE(peers.valid()); EXPECT_FALSE(peers.revocable(managed)); EXPECT_FALSE(peers.admitted(managed));
    EXPECT_FALSE(peers.remember(managed));
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
    EXPECT_FALSE(peers.revocable(managed)); EXPECT_FALSE(peers.admitted(managed));
    EXPECT_TRUE(peers.remember(managed)); EXPECT_TRUE(peers.revocable(managed)); EXPECT_TRUE(peers.admitted(managed));
    ManagedPeers reloaded{directory, ::getuid()};
    EXPECT_TRUE(reloaded.revocable(managed)); EXPECT_TRUE(reloaded.admitted(managed));
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
        EXPECT_TRUE(peers.revocable(managed)) << "revocation of a known bond was lost";
        EXPECT_TRUE(peers.admitted(managed));
        EXPECT_FALSE(peers.revocable(unmanaged)); EXPECT_FALSE(peers.admitted(unmanaged));
        EXPECT_FALSE(peers.revocable(other)) << "a failed commit admitted the peer";
        EXPECT_FALSE(peers.admitted(other));
        ManagedPeers reloaded{directory, ::getuid()};
        EXPECT_FALSE(reloaded.revocable(other)); EXPECT_FALSE(reloaded.admitted(other));
        EXPECT_EQ(leftovers(directory), 0u) << "staging file not removed";
        EXPECT_TRUE(peers.remember(other)) << "a transient failure must not latch";
        EXPECT_TRUE(peers.revocable(managed)); EXPECT_TRUE(peers.revocable(other));
        EXPECT_TRUE(peers.admitted(managed)); EXPECT_TRUE(peers.admitted(other));
        std::filesystem::remove(directory + "/managed-bonds");
    }
}
TEST_F(ManagedBonds, RevalidationFailureRefusesAdmissionOnlyAndRetriesEintr) {
    ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.remember(managed));
    ASSERT_EQ(::chmod(directory.c_str(), 0500), 0);  // directory no longer private: cannot write
    EXPECT_FALSE(peers.remember(other));
    EXPECT_TRUE(peers.revocable(managed)); EXPECT_TRUE(peers.admitted(managed));
    EXPECT_FALSE(peers.revocable(other)); EXPECT_FALSE(peers.admitted(other));
    EXPECT_FALSE(peers.revocable(unmanaged)); EXPECT_FALSE(peers.admitted(unmanaged));
    ASSERT_EQ(::chmod(directory.c_str(), 0700), 0);
    EXPECT_TRUE(peers.remember(other));
    using namespace trust::test::faults;
    { ScopedFault interrupted{Fault::dir_fsync_eintr_once};
      EXPECT_TRUE(peers.remember(unmanaged)) << "EINTR on the directory fsync must be retried"; }
    EXPECT_TRUE(peers.admitted(unmanaged));
}
// Past the rename the entry is on disk: it counts for revocation even when
// the directory fsync fails, but it is not admitted (nor is admission resumed)
// until durability is confirmed by a later successful directory fsync.
TEST_F(ManagedBonds, UncertainDirectorySyncKeepsEntryRevocableAndRefusesAdmission) {
    using namespace trust::test::faults;
    ManagedPeers peers{directory, ::getuid()}; ASSERT_TRUE(peers.remember(other));
    { ScopedFault fail{Fault::dir_fsync}; EXPECT_FALSE(peers.remember(managed));
      EXPECT_TRUE(peers.valid());
      EXPECT_TRUE(peers.revocable(managed)); EXPECT_FALSE(peers.admitted(managed));
      EXPECT_TRUE(peers.revocable(other)); EXPECT_TRUE(peers.admitted(other));
      EXPECT_FALSE(peers.revocable(unmanaged)); EXPECT_FALSE(peers.admitted(unmanaged));
      EXPECT_FALSE(peers.remember(managed)) << "re-admission before durability is confirmed";
      EXPECT_FALSE(peers.remember(unmanaged)); }
    EXPECT_FALSE(peers.admitted(managed)) << "entry remains unadmitted until a directory fsync succeeds";
    EXPECT_FALSE(peers.revocable(unmanaged));
    EXPECT_TRUE(peers.remember(managed)) << "admission resumes once the directory syncs";
    EXPECT_TRUE(peers.admitted(managed));
    EXPECT_TRUE(peers.remember(unmanaged));
    EXPECT_TRUE(peers.admitted(unmanaged));
    ManagedPeers loaded{directory, ::getuid()}; ASSERT_TRUE(loaded.valid());
    EXPECT_TRUE(loaded.revocable(managed)); EXPECT_TRUE(loaded.revocable(other)); EXPECT_TRUE(loaded.revocable(unmanaged));
    EXPECT_TRUE(loaded.admitted(managed)); EXPECT_TRUE(loaded.admitted(other)); EXPECT_TRUE(loaded.admitted(unmanaged));
}
// A forget() rewrite keeps the durable prefix accounting: an entry removed ahead of an admitted
// one never promotes an unadmitted entry, and an uncertain sync never demotes a durable one.
TEST_F(ManagedBonds, ForgetKeepsAdmissionAccountingAcrossUncertainSync) {
    using namespace trust::test::faults;
    ManagedPeers peers{directory, ::getuid()};
    ASSERT_TRUE(peers.remember(other)); ASSERT_TRUE(peers.remember(managed));
    { ScopedFault fail{Fault::dir_fsync};
      EXPECT_FALSE(peers.remember(unmanaged));
      EXPECT_FALSE(peers.forget(other)) << "removal is not durable while the directory sync fails";
      EXPECT_FALSE(peers.revocable(other));
      EXPECT_TRUE(peers.admitted(managed)) << "a durable entry stays admitted";
      EXPECT_TRUE(peers.revocable(unmanaged)); EXPECT_FALSE(peers.admitted(unmanaged)); }
    EXPECT_TRUE(peers.forget(other)) << "the retried sync confirms the removal";
    EXPECT_TRUE(peers.admitted(managed)); EXPECT_TRUE(peers.admitted(unmanaged));
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
    EXPECT_FALSE(peers.valid()); EXPECT_FALSE(peers.revocable(managed)); EXPECT_FALSE(peers.admitted(managed));
    EXPECT_FALSE(peers.remember(managed));
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
    EXPECT_TRUE(peers.revocable({"02:00:00:00:0F:0F", managed.adapter}));
    EXPECT_TRUE(peers.admitted({"02:00:00:00:0F:0F", managed.adapter}));
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
    EXPECT_FALSE(through.valid()); EXPECT_FALSE(through.revocable(managed)); EXPECT_FALSE(through.admitted(managed));
    EXPECT_FALSE(through.remember(other));
    EXPECT_FALSE(through.reason().empty());
    ManagedPeers direct{directory + "/real/state", ::getuid()};
    EXPECT_TRUE(direct.revocable(managed)); EXPECT_TRUE(direct.admitted(managed));
}

struct MockBluez {
    pid_t pid{};
    int in{-1};
    int out{-1};
    explicit MockBluez(const std::string& address) {
        int pipe_out[2]{-1, -1}, pipe_in[2]{-1, -1};
        EXPECT_EQ(::pipe2(pipe_out, O_CLOEXEC), 0);
        EXPECT_EQ(::pipe2(pipe_in, O_CLOEXEC), 0);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipe_out[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipe_in[0], STDIN_FILENO);
        std::string script = std::string{AA_HELPER_SOURCE} + "/tests/helper/bluez_mock.py";
        char py[] = "/usr/bin/python3", flag[] = "-B";
        char* argv[]{py, flag, script.data(), const_cast<char*>(address.c_str()), nullptr};
        const int rc = ::posix_spawn(&pid, py, &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipe_out[1]);
        ::close(pipe_in[0]);
        in = pipe_in[1];
        out = pipe_out[0];
        EXPECT_EQ(rc, 0);
        EXPECT_EQ(read_line(), "ready");
    }
    std::string read_line() const {
        std::string result;
        ::pollfd pfd{out, POLLIN, 0};
        char ch{};
        while (::poll(&pfd, 1, 4000) == 1 && ::read(out, &ch, 1) == 1 && ch != '\n') result.push_back(ch);
        return result;
    }
    std::string command(const std::string& text) const {
        const auto line = text + "\n";
        if (::write(in, line.data(), line.size()) != static_cast<ssize_t>(line.size())) return "write-failed";
        return read_line();
    }
    ~MockBluez() {
        if (in >= 0) ::close(in);
        if (out >= 0) ::close(out);
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, nullptr, 0);
        }
    }
};

std::string file_text(const std::string& path) {
    std::ifstream in{path}; return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}
std::size_t removals(MockBluez& mock) {
    const auto state = mock.command("state");
    const auto at = state.find("\"removed\": [");
    if (at == std::string::npos) return 0;
    const auto end = state.find(']', at);
    std::size_t count = 0;
    for (auto i = state.find("dev_", at); i != std::string::npos && i < end; i = state.find("dev_", i + 1)) ++count;
    return count;
}
// Drives the enrolment up to, but not including, the acknowledgement: authorize, the helper's
// one-time numeric comparison explicitly accepted, the exact bond verified. acknowledge_bond()
// only makes the bond provisional (no write); commit_bond() is the only call that admits it into
// managed-bonds, so a test injects its directory-sync fault around exactly that call (authorize
// also needs a durable pending-removal journal write).
void accept_enrolment(helper::BluezBackend& backend, MockBluez& mock, std::uint64_t handle,
                      const char* start = "comparison-start") {
    using namespace std::chrono_literals;
    ASSERT_TRUE(backend.authorize(handle));
    ASSERT_EQ(mock.command(start), "ok");
    std::optional<helper::PairingComparison> challenge;
    for (int attempt = 0; attempt < 150 && !challenge; ++attempt) {
        if (const auto value = backend.comparison()) challenge = value.value(); else std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(challenge.has_value());
    ASSERT_TRUE(backend.respond_comparison({challenge->id, true}));
    ASSERT_EQ(mock.command("paired"), "ok");
    bool bonded = false;
    for (int attempt = 0; attempt < 150 && !bonded; ++attempt) {
        const auto confirmed = backend.bond_confirmed();
        bonded = confirmed && confirmed.value();
        if (!bonded) std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(bonded);
}

// The receiver's commit after its durable trust write: acknowledgement, then admission.
core::Result<void> admit(helper::BluezBackend& backend) {
    const auto acknowledged = backend.acknowledge_bond();
    if (!acknowledged) return acknowledged;
    return backend.commit_bond();
}

TEST_F(ManagedBonds, DirFsyncFailureAtCommitRefusesAdmissionRemovesBondAndKeepsRevocation) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    const helper::PeerIdentity secondary{"02:00:00:00:00:02", "02:00:00:00:00:AA"};

    // Case 1: an injected dir fsync failure on the commit's managed write refuses the admission;
    // the bond stays unadmitted (no reconnect), cleanup removes it, revocation still succeeds.
    {
        const auto dir1 = directory + "/case1";
        ASSERT_EQ(::mkdir(dir1.c_str(), 0700), 0);
        helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, dir1, ::getuid()};
        ASSERT_TRUE(backend.available());
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(backend.reconcile_bonds({}));
        ASSERT_EQ(mock.command("paired"), "ok");

        const auto first = backend.observe();
        ASSERT_TRUE(first);
        {
            accept_enrolment(backend, mock, first.value().handle);
            if (HasFatalFailure()) return;
            ASSERT_TRUE(backend.acknowledge_bond());
            using namespace trust::test::faults;
            ScopedFault fail{Fault::dir_fsync};
            const auto auth = backend.commit_bond();
            ASSERT_FALSE(auth);
            EXPECT_EQ(auth.error().code(), ErrorCode::helper_rejected);
        }
        {
            // The failed commit closed the window and parked the bond: the receiver's approval may be
            // durable, so nothing removes it before the receiver's reconciliation answers.
            EXPECT_EQ(removals(mock), 0U) << "a parked bond was removed before the receiver answered";
            // The receiver retracted (or never wrote) its approval: reconciliation removes the bond.
            ASSERT_TRUE(backend.reconcile_bonds({}));
            EXPECT_NE(mock.command("state").find("/org/bluez/hci0/dev_02_00_00_00_00_01"), std::string::npos)
                << "a bond whose commit failed was not removed";
            ASSERT_EQ(mock.command("restore"), "ok");
            ASSERT_EQ(mock.command("paired"), "ok");
            const auto reconn_obs = backend.observe();
            ASSERT_TRUE(reconn_obs);
            const auto reconn = backend.reconnect_peer(reconn_obs.value().handle);
            EXPECT_FALSE(reconn) << "reconnect must be refused for a bond whose commit failed";
            if (!reconn) { EXPECT_EQ(reconn.error().code(), ErrorCode::helper_rejected); }

            EXPECT_TRUE(backend.revoke_peer(primary)) << "revocation must still succeed after directory fsync failure";
        }
    }

    // Case 2: a later successful directory fsync (admitting a different peer) never turns the
    // failed commit's remnant into an admission; only a committed bond reconnects, also after restart.
    ASSERT_EQ(mock.command("restore"), "ok");
    ASSERT_EQ(mock.command("paired"), "ok");
    {
        const auto dir2 = directory + "/case2";
        ASSERT_EQ(::mkdir(dir2.c_str(), 0700), 0);
        {
            helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, dir2, ::getuid()};
            ASSERT_TRUE(backend.available());
            ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
            ASSERT_TRUE(backend.reconcile_bonds({}));

            const auto first = backend.observe();
            ASSERT_TRUE(first);
            accept_enrolment(backend, mock, first.value().handle);
            if (HasFatalFailure()) return;
            {
                ASSERT_TRUE(backend.acknowledge_bond());
                using namespace trust::test::faults;
                ScopedFault fail{Fault::dir_fsync};
                ASSERT_FALSE(backend.commit_bond());
            }

            // Admitting another peer succeeds and confirms the directory fsync.
            ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
            ASSERT_EQ(mock.command("restore"), "ok");
            ASSERT_EQ(mock.command("paired"), "ok");
            ASSERT_EQ(mock.command("observe-other"), "ok");
            const auto other_obs = backend.observe();
            ASSERT_TRUE(other_obs);
            accept_enrolment(backend, mock, other_obs.value().handle, "comparison-start wrong");
            if (HasFatalFailure()) return;
            ASSERT_TRUE(admit(backend));
            ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));

            ASSERT_EQ(mock.command("observe-first"), "ok");
            const auto refused_obs = backend.observe();
            ASSERT_TRUE(refused_obs);
            EXPECT_FALSE(backend.reconnect_peer(refused_obs.value().handle))
                << "a later directory fsync must not admit the bond whose commit failed";
        }

        // Across a helper restart with a working directory sync, committed entries loaded from disk are
        // admitted once the receiver's reconciliation (approving only the committed peer) answered.
        helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, dir2, ::getuid()};
        ASSERT_TRUE(restarted.available());
        ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
        const auto first_restart = restarted.observe();
        ASSERT_TRUE(first_restart);
        EXPECT_FALSE(restarted.reconnect_peer(first_restart.value().handle));
        ASSERT_TRUE(restarted.reconcile_bonds({secondary}));
        // The failed commit's remnant is not approved: reconciliation demoted it and removed its bond.
        EXPECT_EQ(file_text(dir2 + "/managed-bonds").find("02:00:00:00:00:01"), std::string::npos);
        EXPECT_NE(file_text(dir2 + "/revoked-bonds").find("02:00:00:00:00:01"), std::string::npos);
        ASSERT_EQ(mock.command("restore"), "ok");  // the mock models one present device at a time
        ASSERT_EQ(mock.command("paired"), "ok");
        ASSERT_EQ(mock.command("observe-other"), "ok");
        const auto after_restart = restarted.observe();
        ASSERT_TRUE(after_restart);
        EXPECT_TRUE(restarted.reconnect_peer(after_restart.value().handle));
        EXPECT_TRUE(restarted.revoke_peer(secondary));
        ASSERT_EQ(mock.command("observe-first"), "ok");
    }
}

TEST_F(ManagedBonds, RestartWithFailingDirFsyncKeepsRevocationRefusesAdmissionAndReconnect) {
    using namespace trust::test::faults;
    const auto peers_dir = directory + "/peers";
    ASSERT_EQ(::mkdir(peers_dir.c_str(), 0700), 0);
    {
        ScopedFault fail{Fault::dir_fsync};
        ManagedPeers before{peers_dir, ::getuid()};
        ASSERT_TRUE(before.valid());
        EXPECT_FALSE(before.remember(managed));
        EXPECT_TRUE(before.revocable(managed));
        EXPECT_FALSE(before.admitted(managed));

        ManagedPeers restarted{peers_dir, ::getuid()};
        ASSERT_TRUE(restarted.valid());
        EXPECT_TRUE(restarted.revocable(managed));
        EXPECT_FALSE(restarted.admitted(managed))
            << "reloaded entry must stay unadmitted while directory fsync fails";
    }

    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    const auto backend_dir = directory + "/backend";
    ASSERT_EQ(::mkdir(backend_dir.c_str(), 0700), 0);
    ASSERT_EQ(mock.command("paired"), "ok");
    {
        {
            helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
            ASSERT_TRUE(initial.available());
            ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
            ASSERT_TRUE(initial.reconcile_bonds({}));
            const auto obs = initial.observe();
            ASSERT_TRUE(obs);
            accept_enrolment(initial, mock, obs.value().handle);
            if (HasFatalFailure()) return;
            ASSERT_TRUE(admit(initial));  // committed; its durability is unconfirmed after restart
        }
        ScopedFault fail{Fault::dir_fsync};

        helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
        ASSERT_TRUE(restarted.available());
        ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
        EXPECT_FALSE(restarted.reconcile_bonds({primary})) << "reconciled although the managed entry is not durable";
        const auto reconn_obs = restarted.observe();
        ASSERT_TRUE(reconn_obs);
        const auto reconn = restarted.reconnect_peer(reconn_obs.value().handle);
        EXPECT_FALSE(reconn) << "reconnect after restart must be refused while directory fsync fails";
        if (!reconn) { EXPECT_EQ(reconn.error().code(), ErrorCode::helper_rejected); }

        // Finding 4: the bond is removed, but a demotion that is not durable is never reported as done.
        EXPECT_FALSE(restarted.revoke_peer(primary)) << "revocation reported success with an unconfirmed demotion";
        EXPECT_NE(mock.command("state").find("/org/bluez/hci0/dev_02_00_00_00_00_01"), std::string::npos);
    }
}

TEST_F(ManagedBonds, RestartWithFailingDirFsyncAllowsReconnectOnceSyncSucceeds) {
    using namespace trust::test::faults;
    const auto peers_dir = directory + "/peers";
    ASSERT_EQ(::mkdir(peers_dir.c_str(), 0700), 0);
    std::unique_ptr<ManagedPeers> reloaded;
    {
        ScopedFault fail{Fault::dir_fsync};
        ManagedPeers before{peers_dir, ::getuid()};
        ASSERT_TRUE(before.valid());
        EXPECT_FALSE(before.remember(managed));

        reloaded = std::make_unique<ManagedPeers>(peers_dir, ::getuid());
        ASSERT_TRUE(reloaded->valid());
        EXPECT_TRUE(reloaded->revocable(managed));
        EXPECT_FALSE(reloaded->admitted(managed));
        EXPECT_FALSE(reloaded->confirm_durable());
        EXPECT_FALSE(reloaded->admitted(managed));
    }
    EXPECT_TRUE(reloaded->confirm_durable());
    EXPECT_TRUE(reloaded->admitted(managed));

    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const auto backend_dir = directory + "/backend";
    ASSERT_EQ(::mkdir(backend_dir.c_str(), 0700), 0);
    ASSERT_EQ(mock.command("paired"), "ok");

    std::unique_ptr<helper::BluezBackend> restarted;
    {
        {
            helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
            ASSERT_TRUE(initial.available());
            ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
            ASSERT_TRUE(initial.reconcile_bonds({}));
            const auto obs = initial.observe();
            ASSERT_TRUE(obs);
            accept_enrolment(initial, mock, obs.value().handle);
            if (HasFatalFailure()) return;
            ASSERT_TRUE(admit(initial));
        }
        ScopedFault fail{Fault::dir_fsync};

        // Restart while the directory fsync is still failing; reconnect must be refused,
        // then allowed on the next reconnect attempt once the directory fsync succeeds.
        restarted = std::make_unique<helper::BluezBackend>(
            bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid());
        ASSERT_TRUE(restarted->available());
        ASSERT_TRUE(restarted->apply(helper::Action::register_profile, 0));
        EXPECT_FALSE(restarted->reconcile_bonds({{"02:00:00:00:00:01", "02:00:00:00:00:AA"}}));
        const auto refused_obs = restarted->observe();
        ASSERT_TRUE(refused_obs);
        const auto refused = restarted->reconnect_peer(refused_obs.value().handle);
        EXPECT_FALSE(refused);
        if (!refused) { EXPECT_EQ(refused.error().code(), ErrorCode::helper_rejected); }
    }

    // The receiver retries its reconciliation; with the sync working it confirms durability.
    ASSERT_TRUE(restarted->reconcile_bonds({{"02:00:00:00:00:01", "02:00:00:00:00:AA"}}));
    const auto allowed_obs = restarted->observe();
    ASSERT_TRUE(allowed_obs);
    EXPECT_TRUE(restarted->reconnect_peer(allowed_obs.value().handle))
        << "next reconnect attempt must confirm durability and succeed without a new pairing";
}

TEST_F(ManagedBonds, NormalRestartAfterSuccessfulSyncAdmitsLoadedEntries) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const auto backend_dir = directory + "/backend";
    ASSERT_EQ(::mkdir(backend_dir.c_str(), 0700), 0);
    ASSERT_EQ(mock.command("paired"), "ok");

    {
        helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
        ASSERT_TRUE(initial.available());
        ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(initial.reconcile_bonds({}));
        const auto obs = initial.observe();
        ASSERT_TRUE(obs);
        accept_enrolment(initial, mock, obs.value().handle);
        if (HasFatalFailure()) return;
        ASSERT_TRUE(admit(initial));
    }

    ManagedPeers reloaded{backend_dir, ::getuid()};
    ASSERT_TRUE(reloaded.valid());
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    EXPECT_TRUE(reloaded.revocable(primary));
    EXPECT_TRUE(reloaded.admitted(primary));

    helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
    ASSERT_TRUE(restarted.available());
    ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
    const auto early = restarted.observe();
    ASSERT_TRUE(early);
    EXPECT_FALSE(restarted.reconnect_peer(early.value().handle)) << "admitted before the first reconciliation";
    ASSERT_TRUE(restarted.reconcile_bonds({primary}));
    const auto reconn_obs = restarted.observe();
    ASSERT_TRUE(reconn_obs);
    EXPECT_TRUE(restarted.reconnect_peer(reconn_obs.value().handle));
}

// Finding B: revocation demotes the admitted bond to history. A fresh enrolment of the same
// adapter/address that is then rejected must have its new bond removed, and the journal entry
// must leave only after that removal.
TEST_F(ManagedBonds, RevokedHistoryNeverSuppressesCleanupOfARejectedReEnrolment) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    const std::string address = primary.address;
    helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
    ASSERT_TRUE(backend.available());
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({}));
    const auto first = backend.observe();
    ASSERT_TRUE(first);
    accept_enrolment(backend, mock, first.value().handle);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(admit(backend));
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    EXPECT_EQ(removals(mock), 0U) << "an admitted bond was removed";
    // Forget: removed, demoted to history.
    ASSERT_TRUE(backend.revoke_peer(primary));
    EXPECT_EQ(removals(mock), 1U);
    EXPECT_EQ(file_text(directory + "/managed-bonds").find(address), std::string::npos) << "revocation left the admission";
    EXPECT_NE(file_text(directory + "/revoked-bonds").find(address), std::string::npos) << "revocation lost its authority";
    // Re-enrol the same identity; the owner accepts, the bond forms and is acknowledged, then the
    // enrolment is cancelled before the commit.
    ASSERT_EQ(mock.command("restore"), "ok");
    ASSERT_EQ(mock.command("unpaired"), "ok");
    const auto again = backend.observe();
    ASSERT_TRUE(again);
    accept_enrolment(backend, mock, again.value().handle);
    if (HasFatalFailure()) return;
    EXPECT_NE(file_text(directory + "/pending-removals").find(address), std::string::npos);
    ASSERT_TRUE(backend.acknowledge_bond());
    const auto reconnect = backend.observe();
    ASSERT_TRUE(reconnect);
    EXPECT_FALSE(backend.reconnect_peer(reconnect.value().handle)) << "history admitted the new bond";
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    EXPECT_EQ(removals(mock), 1U) << "the acknowledged bond was removed before the receiver answered";
    ASSERT_TRUE(backend.reconcile_bonds({}));  // the receiver approves nothing
    EXPECT_EQ(removals(mock), 2U) << "the rejected re-enrolment's bond was kept";
    EXPECT_EQ(file_text(directory + "/pending-removals").find(address), std::string::npos) << "journal entry kept after removal";
    EXPECT_EQ(file_text(directory + "/managed-bonds").find(address), std::string::npos);
    // A retried revocation of the removed bond still completes.
    EXPECT_TRUE(backend.revoke_peer(primary));
}
// Restart before reconciliation: an acknowledged bond whose commit never arrived is neither
// admitted nor removed until the receiver answers; the answer alone decides (finding 1).
class ProvisionalRestart : public ManagedBonds, public testing::WithParamInterface<bool> {};
TEST_P(ProvisionalRestart, ReceiverAnswerDecidesAProvisionalBondAfterHelperRestart) {
    const bool approved = GetParam();
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    {
        helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
        ASSERT_TRUE(initial.available());
        ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(initial.reconcile_bonds({}));
        const auto obs = initial.observe();
        ASSERT_TRUE(obs);
        accept_enrolment(initial, mock, obs.value().handle);
        if (HasFatalFailure()) return;
        ASSERT_TRUE(initial.acknowledge_bond());
        // Receiver loss (or its crash after the durable trust write, before the commit).
        ASSERT_TRUE(initial.apply(helper::Action::close_pairing, 0));
        EXPECT_EQ(removals(mock), 0U) << "an acknowledged bond was removed before the receiver answered";
    }
    EXPECT_FALSE(std::filesystem::exists(directory + "/managed-bonds")) << "a provisional bond was admitted";
    EXPECT_NE(file_text(directory + "/pending-removals").find("02:00:00:00:00:01"), std::string::npos);
    helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
    ASSERT_TRUE(restarted.available());
    ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
    for (int attempt = 0; attempt < 15; ++attempt) {
        restarted.tick(); std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    EXPECT_EQ(removals(mock), 0U) << "cleanup removed a bond before the first reconciliation";
    const auto early = restarted.observe();
    ASSERT_TRUE(early);
    EXPECT_FALSE(restarted.reconnect_peer(early.value().handle)) << "admitted before the first reconciliation";
    EXPECT_FALSE(restarted.authorize(early.value().handle)) << "an enrolment opened before the first reconciliation";
    const auto answer = approved ? restarted.reconcile_bonds({primary}) : restarted.reconcile_bonds({});
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer.value(), 0U);
    if (approved) {
        // Crash window 1: the trust write landed, the commit did not. The approval is kept and the
        // bond admitted.
        EXPECT_EQ(removals(mock), 0U);
        EXPECT_NE(file_text(directory + "/managed-bonds").find("02:00:00:00:00:01"), std::string::npos);
        EXPECT_EQ(file_text(directory + "/pending-removals").find("02:00:00:00:00:01"), std::string::npos);
        const auto obs = restarted.observe();
        ASSERT_TRUE(obs);
        EXPECT_TRUE(restarted.reconnect_peer(obs.value().handle)) << "the approved bond was not admitted";
    } else {
        EXPECT_EQ(removals(mock), 1U) << "the restarted helper kept a bond the receiver never approved";
        EXPECT_EQ(file_text(directory + "/pending-removals").find("02:00:00:00:00:01"), std::string::npos);
        EXPECT_FALSE(std::filesystem::exists(directory + "/managed-bonds") &&
                     file_text(directory + "/managed-bonds").find("02:00:00:00:00:01") != std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(Answer, ProvisionalRestart, testing::Bool());
// Crash window 2: the helper received the commit, durably wrote managed-bonds and stopped before the
// journal entry was durably removed. The restarted helper admits nothing until the receiver's
// reconciliation, which (the phone approved) completes the commit and never removes the bond.
TEST_F(ManagedBonds, CommitInterruptedBetweenItsTwoWritesHealsOnReconciliation) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    {
        helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
        ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(initial.reconcile_bonds({}));
        const auto obs = initial.observe();
        ASSERT_TRUE(obs);
        accept_enrolment(initial, mock, obs.value().handle);
        if (HasFatalFailure()) return;
        ASSERT_TRUE(admit(initial));
    }
    // The durable outcome of a crash between the managed write and the journal removal.
    {
        std::ofstream journal{directory + "/pending-removals", std::ios::trunc};
        journal << "v1\n02:00:00:00:00:01 02:00:00:00:00:AA\n";
    }
    ASSERT_EQ(::chmod((directory + "/pending-removals").c_str(), 0600), 0);
    helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
    ASSERT_TRUE(restarted.available());
    ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
    for (int attempt = 0; attempt < 15; ++attempt) {
        restarted.tick(); std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    EXPECT_EQ(removals(mock), 0U) << "restart cleanup removed an approved bond";
    const auto early = restarted.observe();
    ASSERT_TRUE(early);
    EXPECT_FALSE(restarted.reconnect_peer(early.value().handle));
    ASSERT_TRUE(restarted.reconcile_bonds({primary}));
    EXPECT_EQ(removals(mock), 0U);
    EXPECT_EQ(file_text(directory + "/pending-removals").find("02:00:00:00:00:01"), std::string::npos);
    const auto obs = restarted.observe();
    ASSERT_TRUE(obs);
    EXPECT_TRUE(restarted.reconnect_peer(obs.value().handle)) << "the committed bond was not admitted";
}
// Finding 4: a demotion whose writes are not durable is reported as failed, and a power loss that
// drops its unsynced renames (simulated by restoring the files the renames replaced) cannot
// resurrect the admission: the restarted helper admits nothing until reconciliation, which demotes
// again, and only reports success once that demotion is durable.
TEST_F(ManagedBonds, UnconfirmedDemotionSurvivesPowerLossWithoutResurrectingAdmission) {
    using namespace trust::test::faults;
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    std::string managed_before, revoked_before;
    {
        helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
        ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(initial.reconcile_bonds({}));
        const auto obs = initial.observe();
        ASSERT_TRUE(obs);
        accept_enrolment(initial, mock, obs.value().handle);
        if (HasFatalFailure()) return;
        ASSERT_TRUE(admit(initial));
        ASSERT_TRUE(initial.apply(helper::Action::close_pairing, 0));
        managed_before = file_text(directory + "/managed-bonds");
        revoked_before = std::filesystem::exists(directory + "/revoked-bonds") ? file_text(directory + "/revoked-bonds") : "";
        ScopedFault fail{Fault::dir_fsync};
        EXPECT_FALSE(initial.revoke_peer(primary)) << "a demotion without a confirmed directory sync reported success";
        const auto after = initial.observe();
        if (after) { EXPECT_FALSE(initial.reconnect_peer(after.value().handle)); }
    }
    // Power loss: the unsynced renames are lost.
    {
        std::ofstream{directory + "/managed-bonds", std::ios::trunc} << managed_before;
        if (revoked_before.empty()) std::filesystem::remove(directory + "/revoked-bonds");
        else std::ofstream{directory + "/revoked-bonds", std::ios::trunc} << revoked_before;
    }
    ASSERT_NE(file_text(directory + "/managed-bonds").find("02:00:00:00:00:01"), std::string::npos);
    ASSERT_EQ(mock.command("restore"), "ok");
    ASSERT_EQ(mock.command("paired"), "ok");
    ScopedFault fail{Fault::dir_fsync};
    helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
    ASSERT_TRUE(restarted.available());
    ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
    const auto early = restarted.observe();
    ASSERT_TRUE(early);
    EXPECT_FALSE(restarted.reconnect_peer(early.value().handle)) << "power loss resurrected the admission";
    // The receiver's store no longer approves the phone; the demotion still cannot be made durable.
    EXPECT_FALSE(restarted.reconcile_bonds({}));
    const auto still = restarted.observe();
    if (still) { EXPECT_FALSE(restarted.reconnect_peer(still.value().handle)) << "a failed demotion admitted the bond"; }
    disarm();
    ASSERT_TRUE(restarted.reconcile_bonds({}));
    EXPECT_EQ(file_text(directory + "/managed-bonds").find("02:00:00:00:00:01"), std::string::npos);
    EXPECT_NE(file_text(directory + "/revoked-bonds").find("02:00:00:00:00:01"), std::string::npos);
}
// The revocation outranks an approval that was never committed after it (a resurrected or stale
// approval): the bond is not admitted, and it is not removed either (#74, it is approved).
TEST_F(ManagedBonds, ApprovalOfARevokedBondIsNotAdmittedAndNotRemoved) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    {
        std::ofstream{directory + "/revoked-bonds"} << "v1\n02:00:00:00:00:01 02:00:00:00:00:AA\n";
    }
    ASSERT_EQ(::chmod((directory + "/revoked-bonds").c_str(), 0600), 0);
    ASSERT_EQ(mock.command("paired"), "ok");
    helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    const auto answer = backend.reconcile_bonds({primary});
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer.value(), 1U) << "the refused approval was not reported";
    const auto obs = backend.observe();
    ASSERT_TRUE(obs);
    EXPECT_FALSE(backend.reconnect_peer(obs.value().handle));
    EXPECT_EQ(removals(mock), 0U);
}
TEST_F(ManagedBonds, ReconciliationRefusesMalformedOrOversizedSets) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    EXPECT_FALSE(backend.reconcile_bonds({{"not-an-address", "02:00:00:00:00:AA"}}));
    std::vector<helper::PeerIdentity> oversized;
    for (int i = 0; i < 257; ++i) {
        char address[18]; std::snprintf(address, sizeof address, "02:00:00:00:%02X:%02X", i / 256, i % 256);
        oversized.push_back({address, "02:00:00:00:00:AA"});
    }
    EXPECT_FALSE(backend.reconcile_bonds(oversized));
    const auto obs = backend.observe();
    ASSERT_TRUE(obs);
    EXPECT_FALSE(backend.authorize(obs.value().handle)) << "a refused reconciliation counted";
}
TEST_F(ManagedBonds, RevokedHistoryEvictsItsOldestEntryWhenFull) {
    ManagedPeers history{directory, ::getuid(), "revoked-bonds"};
    ASSERT_TRUE(history.valid());
    auto peer = [](int i) {
        char address[18]; std::snprintf(address, sizeof address, "02:00:00:00:%02X:%02X", i / 256, i % 256);
        return helper::PeerIdentity{address, "00:11:22:33:44:55"};
    };
    for (int i = 0; i < 256; ++i) ASSERT_TRUE(history.remember(peer(i), true));
    EXPECT_FALSE(history.remember(peer(256))) << "a full authority list must refuse without eviction";
    ASSERT_TRUE(history.remember(peer(256), true));
    EXPECT_FALSE(history.contains(peer(0)));
    EXPECT_TRUE(history.contains(peer(1)) && history.contains(peer(256)));
    EXPECT_EQ(history.peers().size(), 256U);
    ManagedPeers reloaded{directory, ::getuid(), "revoked-bonds"};
    EXPECT_TRUE(reloaded.valid() && reloaded.contains(peer(256)) && !reloaded.contains(peer(0)));
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
