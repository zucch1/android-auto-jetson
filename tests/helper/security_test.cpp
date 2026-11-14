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
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

TEST_F(ManagedBonds, DirFsyncFailureRefusesPreBondedReconnectKeepsRevocationAndAllowsAfterSync) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    const helper::PeerIdentity secondary{"02:00:00:00:00:02", "02:00:00:00:00:AA"};

    // Case 1: injected dir fsync failure on remember() refuses reconnect of the
    // pre-bonded identity, while revocation still succeeds.
    {
        const auto dir1 = directory + "/case1";
        ASSERT_EQ(::mkdir(dir1.c_str(), 0700), 0);
        helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, dir1, ::getuid()};
        ASSERT_TRUE(backend.available());
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        ASSERT_EQ(mock.command("paired"), "ok");

        const auto first = backend.observe();
        ASSERT_TRUE(first);
        {
            using namespace trust::test::faults;
            ScopedFault fail{Fault::dir_fsync};
            const auto auth = backend.authorize(first.value().handle);
            ASSERT_FALSE(auth);
            EXPECT_EQ(auth.error().code(), ErrorCode::helper_rejected);

            const auto reconn_obs = backend.observe();
            ASSERT_TRUE(reconn_obs);
            const auto reconn = backend.reconnect_peer(reconn_obs.value().handle);
            EXPECT_FALSE(reconn) << "reconnect must be refused while directory fsync fails";
            if (!reconn) { EXPECT_EQ(reconn.error().code(), ErrorCode::helper_rejected); }

            EXPECT_TRUE(backend.revoke_peer(primary)) << "revocation must still succeed after directory fsync failure";
        }
        EXPECT_NE(mock.command("state").find("/org/bluez/hci0/dev_02_00_00_00_00_01"), std::string::npos);
    }

    // Case 2: after a later successful directory fsync (even while admitting a
    // different peer), reconnect of the pre-bonded identity becomes allowed.
    ASSERT_EQ(mock.command("restore"), "ok");
    ASSERT_EQ(mock.command("paired"), "ok");
    {
        const auto dir2 = directory + "/case2";
        ASSERT_EQ(::mkdir(dir2.c_str(), 0700), 0);
        {
            helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, dir2, ::getuid()};
            ASSERT_TRUE(backend.available());
            ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));

            const auto first = backend.observe();
            ASSERT_TRUE(first);
            {
                using namespace trust::test::faults;
                ScopedFault fail{Fault::dir_fsync};
                ASSERT_FALSE(backend.authorize(first.value().handle));

                const auto refused_obs = backend.observe();
                ASSERT_TRUE(refused_obs);
                EXPECT_FALSE(backend.reconnect_peer(refused_obs.value().handle));
            }

            // Admitting another peer succeeds and confirms the directory fsync.
            ASSERT_EQ(mock.command("observe-other"), "ok");
            const auto other_obs = backend.observe();
            ASSERT_TRUE(other_obs);
            ASSERT_TRUE(backend.authorize(other_obs.value().handle));
            ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));

            ASSERT_EQ(mock.command("observe-first"), "ok");
            const auto allowed_obs = backend.observe();
            ASSERT_TRUE(allowed_obs);
            EXPECT_TRUE(backend.reconnect_peer(allowed_obs.value().handle))
                << "reconnect should succeed once a later directory fsync confirms durability";
        }

        // Across a helper restart with a working directory sync, entries loaded from disk are admitted.
        helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, dir2, ::getuid()};
        ASSERT_TRUE(restarted.available());
        ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
        const auto after_restart = restarted.observe();
        ASSERT_TRUE(after_restart);
        EXPECT_TRUE(restarted.reconnect_peer(after_restart.value().handle));
        ASSERT_EQ(mock.command("observe-other"), "ok");
        EXPECT_TRUE(restarted.revoke_peer(secondary));
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
        ScopedFault fail{Fault::dir_fsync};
        {
            helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
            ASSERT_TRUE(initial.available());
            ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
            const auto obs = initial.observe();
            ASSERT_TRUE(obs);
            const auto auth = initial.authorize(obs.value().handle);
            ASSERT_FALSE(auth);
            EXPECT_EQ(auth.error().code(), ErrorCode::helper_rejected);
        }

        helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
        ASSERT_TRUE(restarted.available());
        ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
        const auto reconn_obs = restarted.observe();
        ASSERT_TRUE(reconn_obs);
        const auto reconn = restarted.reconnect_peer(reconn_obs.value().handle);
        EXPECT_FALSE(reconn) << "reconnect after restart must be refused while directory fsync fails";
        if (!reconn) { EXPECT_EQ(reconn.error().code(), ErrorCode::helper_rejected); }

        EXPECT_TRUE(restarted.revoke_peer(primary)) << "revocation must still work when durability is unconfirmed";
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
        ScopedFault fail{Fault::dir_fsync};
        {
            helper::BluezBackend initial{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
            ASSERT_TRUE(initial.available());
            ASSERT_TRUE(initial.apply(helper::Action::register_profile, 0));
            const auto obs = initial.observe();
            ASSERT_TRUE(obs);
            ASSERT_FALSE(initial.authorize(obs.value().handle));
        }

        // Restart while the directory fsync is still failing; reconnect must be refused,
        // then allowed on the next reconnect attempt once the directory fsync succeeds.
        restarted = std::make_unique<helper::BluezBackend>(
            bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid());
        ASSERT_TRUE(restarted->available());
        ASSERT_TRUE(restarted->apply(helper::Action::register_profile, 0));
        const auto refused_obs = restarted->observe();
        ASSERT_TRUE(refused_obs);
        const auto refused = restarted->reconnect_peer(refused_obs.value().handle);
        EXPECT_FALSE(refused);
        if (!refused) { EXPECT_EQ(refused.error().code(), ErrorCode::helper_rejected); }
    }

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
        const auto obs = initial.observe();
        ASSERT_TRUE(obs);
        ASSERT_TRUE(initial.authorize(obs.value().handle));
    }

    ManagedPeers reloaded{backend_dir, ::getuid()};
    ASSERT_TRUE(reloaded.valid());
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    EXPECT_TRUE(reloaded.revocable(primary));
    EXPECT_TRUE(reloaded.admitted(primary));

    helper::BluezBackend restarted{bus_address, [] { return std::chrono::steady_clock::now(); }, backend_dir, ::getuid()};
    ASSERT_TRUE(restarted.available());
    ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
    const auto reconn_obs = restarted.observe();
    ASSERT_TRUE(reconn_obs);
    EXPECT_TRUE(restarted.reconnect_peer(reconn_obs.value().handle));
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
