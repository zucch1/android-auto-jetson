// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/helper/bluez_adapter/ManagedPeers.hpp"
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <aa/helper/BluezBackend.hpp>
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <gtest/gtest.h>
#include "../trust/syscall_faults.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
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
        // Private (0700, mkdtemp) and inside the build tree, never under /tmp or a shared tree.
        auto pattern = (std::filesystem::path{AA_HELPER_BINARY_DIR} / "aa-bonds-XXXXXX").string();
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
        // The adapter is shared (#136): no scenario may remove, cancel or disconnect a pairing.
        if (in >= 0 && pid > 0) {
            const auto state = command("state");
            const auto at = state.find("\"destructive\": ");
            if (at != std::string::npos) {
                EXPECT_EQ(std::stoul(state.substr(at + 15)), 0U)
                    << "the helper called RemoveDevice, CancelPairing or Disconnect: " << state;
            }
        }
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
// A counter from the mock's state (remove_attempts, extras).
std::size_t state_number(MockBluez& mock, const char* key) {
    const auto state = mock.command("state");
    const auto at = state.find(std::string{"\""} + key + "\": ");
    return at == std::string::npos ? 0U : std::stoul(state.substr(at + std::string{key}.size() + 4));
}
// Device1.ConnectProfile(HFP Audio Gateway) pages of the primary device the mock received. The
// helper sends them without awaiting a reply, so a positive count is polled for.
std::size_t hands_free_pages(MockBluez& mock, std::size_t at_least = 0) {
    using namespace std::chrono_literals;
    const std::string page = "[\"/org/bluez/hci0/dev_02_00_00_00_00_01\", \"0000111f-0000-1000-8000-00805f9b34fb\"]";
    std::size_t count = 0;
    for (int attempt = 0; attempt < (at_least ? 100 : 5); ++attempt) {
        const auto state = mock.command("state");
        count = 0;
        for (auto i = state.find(page); i != std::string::npos; i = state.find(page, i + 1)) ++count;
        if (at_least && count >= at_least) return count;
        std::this_thread::sleep_for(20ms);
    }
    return count;
}
const helper::PeerIdentity primary_peer{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
bool peer_in(const std::string& directory, const char* file) {
    return std::filesystem::exists(directory + "/" + file) &&
           file_text(directory + "/" + file).find(primary_peer.address) != std::string::npos;
}
// Every Android Auto path refuses a revoked identity whose bond is still in BlueZ: reconnect
// (ReconnectPeerV1), the Android Auto profile's RFCOMM connection and service authorization
// (bootstrap and HFP via Android Auto). None of them removes the bond.
// A BlueZ callback into the in-process helper: the mock calls it synchronously, so the helper's
// context is pumped (ticks) until the mock reports the answer.
std::string bluez_callback(helper::BluezBackend& backend, MockBluez& mock, const char* text) {
    auto reply = std::async(std::launch::async, [&mock, text] { return mock.command(text); });
    while (reply.wait_for(std::chrono::milliseconds{5}) != std::future_status::ready) backend.tick();
    return reply.get();
}
void expect_refused_everywhere(helper::BluezBackend& backend, MockBluez& mock) {
    const auto obs = backend.observe();
    ASSERT_TRUE(obs);
    EXPECT_FALSE(backend.reconnect_peer(obs.value().handle)) << "a forgotten phone reconnected";
    EXPECT_EQ(hands_free_pages(mock), 0U) << "a forgotten phone was paged";
    EXPECT_EQ(bluez_callback(backend, mock, "NewConnection"), "rejected")
        << "a forgotten phone got the Android Auto profile";
    EXPECT_EQ(bluez_callback(backend, mock, "AuthorizeService"), "rejected")
        << "a forgotten phone got the Android Auto service";
    EXPECT_EQ(bluez_callback(backend, mock, "AuthorizeService hfp"), "rejected")
        << "a forgotten phone got HFP via Android Auto";
    EXPECT_EQ(state_number(mock, "destructive"), 0U) << "a refused bond was removed, cancelled or disconnected";
    EXPECT_NE(mock.command("state").find("\"bonded\": true"), std::string::npos) << "a refused bond is gone";
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

TEST_F(ManagedBonds, DirFsyncFailureAtCommitRefusesAdmissionKeepsBondAndRevocation) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const helper::PeerIdentity primary{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
    const helper::PeerIdentity secondary{"02:00:00:00:00:02", "02:00:00:00:00:AA"};

    // Case 1: an injected dir fsync failure on the commit's managed write refuses the admission;
    // the bond stays unadmitted (no reconnect) and in BlueZ (#136), revocation still succeeds.
    {
        const auto dir1 = directory + "/case1";
        ASSERT_EQ(::mkdir(dir1.c_str(), 0700), 0);
        helper::BluezBackend backend{bus_address, [] { return std::chrono::steady_clock::now(); }, dir1, ::getuid()};
        ASSERT_TRUE(backend.available());
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(backend.reconcile_bonds({}));

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
            // durable, so only the receiver's answer decides; the receiver revokes it.
            EXPECT_TRUE(backend.revoke_peer(primary)) << "revocation must still succeed after directory fsync failure";
            EXPECT_FALSE(peer_in(dir1, "pending-removals"));
            ASSERT_TRUE(backend.reconcile_bonds({}));
            EXPECT_EQ(removals(mock), 0U) << "a bond whose commit failed was removed";
            const auto reconn_obs = backend.observe();
            ASSERT_TRUE(reconn_obs);
            const auto reconn = backend.reconnect_peer(reconn_obs.value().handle);
            EXPECT_FALSE(reconn) << "reconnect must be refused for a bond whose commit failed";
            if (!reconn) { EXPECT_EQ(reconn.error().code(), ErrorCode::helper_rejected); }
        }
    }

    // Case 2: a later successful directory fsync (admitting a different peer) never turns the
    // failed commit's remnant into an admission; only a committed bond reconnects, also after restart.
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

        // Finding 4: a demotion that is not durable is never reported as done. The bond stays in
        // BlueZ either way (shared adapter, #136).
        EXPECT_FALSE(restarted.revoke_peer(primary)) << "revocation reported success with an unconfirmed demotion";
        EXPECT_EQ(removals(mock), 0U) << "a forgotten bond was deleted outside an enrolment window";
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
    EXPECT_EQ(hands_free_pages(mock), 0U) << "a refused reconnect paged the phone";
    ASSERT_TRUE(restarted.reconcile_bonds({primary}));
    const auto reconn_obs = restarted.observe();
    ASSERT_TRUE(reconn_obs);
    EXPECT_TRUE(restarted.reconnect_peer(reconn_obs.value().handle));
    // A bonded phone's Android Auto dials the AA RFCOMM profile only over a live link: the
    // approved reconnect pages exactly that phone's HFP Audio Gateway, never generic Connect.
    EXPECT_EQ(hands_free_pages(mock, 1), 1U) << mock.command("state");
}

// Finding B: revocation demotes the admitted bond to history and keeps it in BlueZ (#136). A fresh
// enrolment of the same adapter/address that is acknowledged and then not approved keeps its new
// bond too: never removed, its journal entry released, the identity refused.
TEST_F(ManagedBonds, RejectedReEnrolmentAfterAForgetKeepsTheNewBondRefused) {
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
    // Forget: demoted to history, the bond kept (shared adapter, #136).
    ASSERT_TRUE(backend.revoke_peer(primary));
    EXPECT_EQ(removals(mock), 0U) << "a forgotten bond was deleted outside an enrolment window";
    EXPECT_EQ(file_text(directory + "/managed-bonds").find(address), std::string::npos) << "revocation left the admission";
    EXPECT_NE(file_text(directory + "/revoked-bonds").find(address), std::string::npos) << "revocation lost its authority";
    // The owner unpairs it outside Android Auto, then re-enrols the same identity; the owner
    // accepts, the bond forms and is acknowledged, then the enrolment is cancelled before the commit.
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
    EXPECT_EQ(removals(mock), 0U) << "the acknowledged bond was removed before the receiver answered";
    ASSERT_TRUE(backend.reconcile_bonds({}));  // the receiver approves nothing
    EXPECT_EQ(file_text(directory + "/pending-removals").find(address), std::string::npos) << "journal entry kept";
    EXPECT_EQ(file_text(directory + "/managed-bonds").find(address), std::string::npos);
    expect_refused_everywhere(backend, mock);
    // A retried revocation still completes.
    EXPECT_TRUE(backend.revoke_peer(primary));
}
// Restart before reconciliation: an acknowledged bond whose commit never arrived is not admitted
// until the receiver answers; the answer alone decides (finding 1). It is never removed (#136).
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
        // Never approved: the entry is released, the bond kept and refused (#136).
        EXPECT_EQ(file_text(directory + "/pending-removals").find("02:00:00:00:00:01"), std::string::npos);
        EXPECT_FALSE(std::filesystem::exists(directory + "/managed-bonds") &&
                     file_text(directory + "/managed-bonds").find("02:00:00:00:00:01") != std::string::npos);
        expect_refused_everywhere(restarted, mock);
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
// approval): the bond is not admitted, and it is not removed either (#74, it is approved). The
// refusal is reported like every other kept revoked bond: a warning naming only the redacted address.
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
    testing::internal::CaptureStderr();
    const auto answer = backend.reconcile_bonds({primary});
    const auto log = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer.value(), 1U) << "the refused approval was not reported";
    EXPECT_NE(log.find("warning bond_kept_revoked_identity id-"), std::string::npos) << log;
    EXPECT_EQ(log.find(primary.address), std::string::npos) << "the raw address was logged";
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

// --- Forgetting and kept bonds on the shared adapter (#136). ---
helper::BluezBackend::Now steady() { return [] { return std::chrono::steady_clock::now(); }; }
// Enrols and commits the primary phone (receiver approval included), then closes the window.
void enrol_admitted(const char* bus_address, MockBluez& mock, const std::string& directory) {
    helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({}));
    const auto obs = backend.observe();
    ASSERT_TRUE(obs);
    accept_enrolment(backend, mock, obs.value().handle);
    if (testing::Test::HasFatalFailure()) return;
    ASSERT_TRUE(admit(backend));
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    ASSERT_TRUE(peer_in(directory, "managed-bonds"));
}
// After a restart the receiver no longer approves the phone: no admission may remain and nothing
// may be owed, but its bond stays in BlueZ (shared adapter, #136), refused, with a warning that
// names only its redacted address.
void expect_forgotten_and_kept(const char* bus_address, MockBluez& mock, const std::string& directory) {
    helper::BluezBackend restarted{bus_address, steady(), directory, ::getuid()};
    ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
    testing::internal::CaptureStderr();
    const auto reconciled = restarted.reconcile_bonds({});
    const auto log = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(reconciled) << "reconciliation reported failure";
    EXPECT_EQ(removals(mock), 0U) << "a forgotten bond was deleted outside an enrolment window";
    EXPECT_NE(log.find("warning bond_kept_revoked_identity id-"), std::string::npos) << log;
    EXPECT_EQ(log.find(primary_peer.address), std::string::npos) << "the raw address was logged";
    EXPECT_FALSE(peer_in(directory, "managed-bonds")) << "an approval survived the forget";
    EXPECT_TRUE(peer_in(directory, "revoked-bonds")) << "the forget lost its history";
    EXPECT_FALSE(peer_in(directory, "pending-removals")) << "a removal is owed outside an enrolment";
    ASSERT_TRUE(restarted.reconcile_bonds({}));
    expect_refused_everywhere(restarted, mock);
}
// ForgetPhone's helper leg (RevokePeerV1) on the shared adapter (#136): the admitted phone is demoted
// to history and refused at once, its bond kept; no removal is owed, so a failing journal or a
// refusing BlueZ changes nothing, and neither does a restart and the receiver's reconciliation.
TEST_F(ManagedBonds, ForgetKeepsTheBondRefusedAndOwesNoRemoval) {
    using namespace trust::test::faults;
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    enrol_admitted(bus_address, mock, directory);
    if (HasFatalFailure()) return;
    {
        helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(backend.reconcile_bonds({primary_peer}));
        // Every rename of the journal would fail, and so would BlueZ's RemoveDevice: neither is used.
        const auto dir = directory;
        set_observer([dir] {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator{dir, error})
                if (entry.path().filename().string().starts_with(".pending-removals-")) arm(Fault::renameat);
        });
        ASSERT_EQ(mock.command("fail-remove"), "ok");
        EXPECT_TRUE(backend.revoke_peer(primary_peer)) << "a forget needs no journal write and no removal";
        set_observer(nullptr);
        disarm();
        EXPECT_EQ(state_number(mock, "remove_attempts"), 0U) << "the forget tried to delete the bond";
        EXPECT_FALSE(peer_in(directory, "managed-bonds"));
        EXPECT_TRUE(peer_in(directory, "revoked-bonds"));
        EXPECT_FALSE(peer_in(directory, "pending-removals")) << "the forget owes a removal";
        expect_refused_everywhere(backend, mock);
        for (int attempt = 0; attempt < 5; ++attempt) backend.tick();
        EXPECT_TRUE(backend.revoke_peer(primary_peer)) << "a retried forget did not complete";
        EXPECT_EQ(state_number(mock, "remove_attempts"), 0U);
    }
    ASSERT_EQ(mock.command("allow-remove"), "ok");
    expect_forgotten_and_kept(bus_address, mock, directory);
}
// The receiver's reconciliation forgets an admitted phone (ForgetPhone while the helper was down):
// demoted and refused, its bond kept, nothing owed.
TEST_F(ManagedBonds, ReconciliationDemotesAnUnapprovedManagedBondAndKeepsIt) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    enrol_admitted(bus_address, mock, directory);
    if (HasFatalFailure()) return;
    expect_forgotten_and_kept(bus_address, mock, directory);
}
// History only (a forget completed earlier, or by an older helper whose journal entry was lost),
// the bond still in BlueZ: the receiver's approval keeps it refused (#74) and so does its absence;
// no reconciliation derives a removal from history any more (#136).
TEST_F(ManagedBonds, ReconciliationNeverDerivesARemovalFromRevocationHistory) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    {
        std::ofstream{directory + "/revoked-bonds"} << "v1\n02:00:00:00:00:01 02:00:00:00:00:AA\n";
    }
    ASSERT_EQ(::chmod((directory + "/revoked-bonds").c_str(), 0600), 0);
    ASSERT_EQ(mock.command("paired"), "ok");
    {
        helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        const auto kept = backend.reconcile_bonds({primary_peer});
        ASSERT_TRUE(kept);
        EXPECT_EQ(kept.value(), 1U);
        EXPECT_EQ(removals(mock), 0U) << "an approved bond was removed (#74)";
    }
    expect_forgotten_and_kept(bus_address, mock, directory);
}
// The adapter is shared (#136): a forgotten phone paired again outside Android Auto is never
// removed. Every reconciliation that finds it bonded logs a warning that names only its redacted
// address and tells the owner to enrol it again; every Android Auto path refuses it, also across
// ticks and a helper restart.
TEST_F(ManagedBonds, ForgottenPhonePairedOutsideAndroidAutoIsRefusedAndKeptWithAWarningEachTime) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    enrol_admitted(bus_address, mock, directory);
    if (HasFatalFailure()) return;
    {
        helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(backend.reconcile_bonds({primary_peer}));
        ASSERT_TRUE(backend.revoke_peer(primary_peer));  // ForgetPhone
        for (std::size_t pairing = 1; pairing <= 2; ++pairing) {
            // The owner unpairs and pairs the phone again outside Android Auto.
            ASSERT_EQ(mock.command("unpaired"), "ok");
            ASSERT_EQ(mock.command("paired"), "ok");
            // The second time the receiver's store still (stale) approves the phone: it is refused
            // and reported all the same.
            const auto approval = pairing == 2 ? std::vector<helper::PeerIdentity>{primary_peer}
                                               : std::vector<helper::PeerIdentity>{};
            testing::internal::CaptureStderr();
            const auto reconciled = backend.reconcile_bonds(approval);
            const auto log = testing::internal::GetCapturedStderr();
            ASSERT_TRUE(reconciled);
            EXPECT_EQ(reconciled.value(), approval.size()) << "the refused approval was not reported";
            EXPECT_NE(log.find("warning bond_kept_revoked_identity id-"), std::string::npos) << log;
            EXPECT_NE(log.find("forgotten in Android Auto"), std::string::npos) << log;
            EXPECT_NE(log.find("unpair it in Bluetooth settings first, then enrol it again"), std::string::npos) << log;
            EXPECT_EQ(log.find("02:00:00:00:00:01"), std::string::npos) << "the raw address was logged";
            EXPECT_FALSE(peer_in(directory, "pending-removals")) << "a removal is owed outside an enrolment";
            for (int attempt = 0; attempt < 5; ++attempt) backend.tick();
            expect_refused_everywhere(backend, mock);
            EXPECT_EQ(state_number(mock, "remove_attempts"), 0U) << "the outside re-bond was targeted for removal";
        }
        // Unpaired outside Android Auto: no warning.
        ASSERT_EQ(mock.command("unpaired"), "ok");
        testing::internal::CaptureStderr();
        ASSERT_TRUE(backend.reconcile_bonds({}));
        EXPECT_EQ(testing::internal::GetCapturedStderr().find("bond_kept_revoked_identity"), std::string::npos);
        ASSERT_EQ(mock.command("paired"), "ok");
    }
    expect_forgotten_and_kept(bus_address, mock, directory);
}
// An enrolment window that opens for a phone BlueZ already has paired (here a forgotten phone whose
// bond was kept) never touches that pairing (#136). Whether the window is cancelled, its number
// rejected, its adapter setup fails, or the helper stops with the window open and restarts, the bond
// is never cancelled, disconnected or removed, the journal entry is released, the phone refused.
enum class PreBondedEnd { cancel, reject, setup_failure, restart };
class PreExistingBond : public ManagedBonds, public testing::WithParamInterface<PreBondedEnd> {};
TEST_P(PreExistingBond, SurvivesAnEnrolmentThatIsCancelledOrFails) {
    using namespace std::chrono_literals;
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    enrol_admitted(bus_address, mock, directory);
    if (HasFatalFailure()) return;
    std::string journal;
    {
        helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(backend.reconcile_bonds({primary_peer}));
        ASSERT_TRUE(backend.revoke_peer(primary_peer));  // ForgetPhone: the bond is kept
        ASSERT_TRUE(backend.reconcile_bonds({}));
        const auto obs = backend.observe();
        ASSERT_TRUE(obs);
        if (GetParam() == PreBondedEnd::setup_failure) {
            ASSERT_EQ(mock.command("fail-set"), "ok");
            EXPECT_FALSE(backend.authorize(obs.value().handle)) << "an enrolment opened with a failing adapter";
            ASSERT_EQ(mock.command("allow-set"), "ok");
        } else {
            ASSERT_TRUE(backend.authorize(obs.value().handle)) << "re-enrolment of a kept bond refused";
            EXPECT_TRUE(peer_in(directory, "pending-removals"));
            ASSERT_EQ(mock.command("comparison-start"), "ok");
            std::optional<helper::PairingComparison> challenge;
            for (int attempt = 0; attempt < 150 && !challenge; ++attempt) {
                if (const auto value = backend.comparison()) challenge = value.value(); else std::this_thread::sleep_for(20ms);
            }
            ASSERT_TRUE(challenge.has_value());
            switch (GetParam()) {
            case PreBondedEnd::cancel: ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0)); break;
            case PreBondedEnd::reject: ASSERT_TRUE(backend.respond_comparison({challenge->id, false})); break;
            default:
                // The helper stops with the window open: the durable journal is what a crash leaves.
                journal = file_text(directory + "/pending-removals");
                break;
            }
        }
        for (int attempt = 0; attempt < 5; ++attempt) backend.tick();
        if (GetParam() != PreBondedEnd::restart) {
            ASSERT_TRUE(backend.reconcile_bonds({}));
            for (int attempt = 0; attempt < 5; ++attempt) backend.tick();
            EXPECT_FALSE(peer_in(directory, "pending-removals")) << "the released entry is still journalled";
            expect_refused_everywhere(backend, mock);
        }
    }
    if (GetParam() == PreBondedEnd::restart) {
        std::ofstream{directory + "/pending-removals", std::ios::trunc} << journal;
        ASSERT_TRUE(peer_in(directory, "pending-removals"));
    }
    helper::BluezBackend restarted{bus_address, steady(), directory, ::getuid()};
    ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
    for (int attempt = 0; attempt < 5; ++attempt) restarted.tick();
    testing::internal::CaptureStderr();
    const auto reconciled = restarted.reconcile_bonds({});
    const auto log = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(reconciled);
    EXPECT_EQ(log.find(primary_peer.address), std::string::npos) << "the raw address was logged";
    for (int attempt = 0; attempt < 5; ++attempt) restarted.tick();
    EXPECT_FALSE(peer_in(directory, "pending-removals")) << "the released entry is still journalled";
    EXPECT_TRUE(peer_in(directory, "revoked-bonds"));
    expect_refused_everywhere(restarted, mock);
}
INSTANTIATE_TEST_SUITE_P(EachEnd, PreExistingBond,
                         testing::Values(PreBondedEnd::cancel, PreBondedEnd::reject, PreBondedEnd::setup_failure,
                                         PreBondedEnd::restart));
// Journal entries an older, exclusive-adapter helper wrote as removal obligations (its demotion
// journalled the removal first). After an upgrade, with no enrolment window open, none of them may
// lead to a Disconnect, CancelPairing or RemoveDevice (#136): not on ticks, reconciliation (the first
// run), RevokePeerV1 with the entry still journalled (the second run) or another restart. Each is
// released, the bond kept, the phone refused, the address redacted.
enum class Legacy { journal_only, journal_and_managed, journal_and_history };
class LegacyJournal : public ManagedBonds, public testing::WithParamInterface<Legacy> {};
TEST_P(LegacyJournal, EntryNeverRemovesABondAfterAnUpgrade) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    const auto seed = [&](const char* file) {
        std::ofstream{directory + "/" + file} << "v1\n02:00:00:00:00:01 02:00:00:00:00:AA\n";
        ASSERT_EQ(::chmod((directory + "/" + file).c_str(), 0600), 0);
    };
    ASSERT_EQ(mock.command("paired"), "ok");
    for (int run = 0; run < 2; ++run) {
        seed("pending-removals");
        if (HasFatalFailure()) return;
        if (GetParam() == Legacy::journal_and_managed) seed("managed-bonds");
        if (GetParam() == Legacy::journal_and_history) seed("revoked-bonds");
        helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        for (int attempt = 0; attempt < 5; ++attempt) backend.tick();
        if (run == 1) {
            // RevokePeerV1 while the legacy entry is still journalled: the entry is authority, the
            // revocation completes without removing, cancelling or disconnecting the pairing.
            ASSERT_TRUE(peer_in(directory, "pending-removals"));
            EXPECT_TRUE(backend.revoke_peer(primary_peer));
            EXPECT_FALSE(peer_in(directory, "pending-removals")) << "the revoked legacy entry is still journalled";
        }
        testing::internal::CaptureStderr();
        const auto reconciled = backend.reconcile_bonds({});
        const auto log = testing::internal::GetCapturedStderr();
        ASSERT_TRUE(reconciled);
        if (run == 0 || GetParam() != Legacy::journal_only) {
            const char* warning = GetParam() == Legacy::journal_only ? "warning bond_kept_unowned_enrolment id-"
                                                                     : "warning bond_kept_revoked_identity id-";
            EXPECT_NE(log.find(warning), std::string::npos) << log;
        }
        EXPECT_EQ(log.find(primary_peer.address), std::string::npos) << "the raw address was logged";
        for (int attempt = 0; attempt < 5; ++attempt) backend.tick();
        ASSERT_TRUE(backend.reconcile_bonds({}));
        EXPECT_FALSE(peer_in(directory, "pending-removals")) << "the legacy entry is still journalled";
        EXPECT_FALSE(peer_in(directory, "managed-bonds")) << "an unapproved legacy bond stayed admitted";
        expect_refused_everywhere(backend, mock);
        if (HasFatalFailure()) return;
    }
}
INSTANTIATE_TEST_SUITE_P(EachShape, LegacyJournal,
                         testing::Values(Legacy::journal_only, Legacy::journal_and_managed, Legacy::journal_and_history));
// A bond the enrolment window itself made, and every race around it (#136): the window ends without
// a commit (cancelled, number rejected, timed out, acknowledged but never committed and the helper
// restarted, or the helper crashed with the window open), the owner then unpairs and pairs the phone
// again outside Android Auto, and ticks, reconciliation, RevokePeerV1 and another restart follow. No
// pairing is ever removed, cancelled or disconnected (the mock counts every such call); a pending
// comparison is rejected through the agent's reply; the bond stays, refused on every Android Auto
// path, with a redacted warning. A later full enrolment of the phone is still admitted.
enum class WindowEnd { cancel, reject, timeout, acknowledged_restart, crash_restart };
class WindowBond : public ManagedBonds, public testing::WithParamInterface<WindowEnd> {};
TEST_P(WindowBond, IsKeptAndRefusedWhateverEndsTheWindow) {
    using namespace std::chrono_literals;
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    auto offset = std::make_shared<std::chrono::steady_clock::duration>(0s);
    const helper::BluezBackend::Now clock = [offset] { return std::chrono::steady_clock::now() + *offset; };
    std::string journal;
    {
        helper::BluezBackend backend{bus_address, clock, directory, ::getuid()};
        ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
        ASSERT_TRUE(backend.reconcile_bonds({}));
        const auto obs = backend.observe();
        ASSERT_TRUE(obs);
        ASSERT_TRUE(backend.authorize(obs.value().handle));
        ASSERT_EQ(mock.command("comparison-start"), "ok");
        std::optional<helper::PairingComparison> challenge;
        for (int attempt = 0; attempt < 150 && !challenge; ++attempt) {
            if (const auto value = backend.comparison()) challenge = value.value(); else std::this_thread::sleep_for(20ms);
        }
        ASSERT_TRUE(challenge.has_value());
        testing::internal::CaptureStderr();
        switch (GetParam()) {
        case WindowEnd::cancel:
            ASSERT_EQ(mock.command("paired"), "ok");  // the bond completes inside the window
            ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
            break;
        case WindowEnd::reject:
            ASSERT_TRUE(backend.respond_comparison({challenge->id, false}));
            ASSERT_EQ(mock.command("paired"), "ok");  // the pairing completes regardless
            for (int attempt = 0; attempt < 5; ++attempt) backend.tick();
            break;
        case WindowEnd::timeout:
            ASSERT_EQ(mock.command("paired"), "ok");
            *offset = 61s;
            for (int attempt = 0; attempt < 5; ++attempt) { backend.tick(); std::this_thread::sleep_for(20ms); }
            break;
        case WindowEnd::acknowledged_restart: {
            ASSERT_TRUE(backend.respond_comparison({challenge->id, true}));
            ASSERT_EQ(mock.command("paired"), "ok");
            bool bonded = false;
            for (int attempt = 0; attempt < 150 && !bonded; ++attempt) {
                const auto confirmed = backend.bond_confirmed();
                bonded = confirmed && confirmed.value();
                if (!bonded) std::this_thread::sleep_for(20ms);
            }
            ASSERT_TRUE(bonded);
            ASSERT_TRUE(backend.acknowledge_bond());  // the receiver then exits before its commit
            break;
        }
        case WindowEnd::crash_restart:
            ASSERT_EQ(mock.command("paired"), "ok");
            journal = file_text(directory + "/pending-removals");  // what a crash leaves on disk
            break;
        }
        const auto log = testing::internal::GetCapturedStderr();
        if (GetParam() == WindowEnd::cancel || GetParam() == WindowEnd::timeout) {
            EXPECT_EQ(mock.command("comparison-result"), "rejected") << "the comparison was not rejected by the agent";
            EXPECT_FALSE(peer_in(directory, "pending-removals")) << "the closed window's entry is still journalled";
            EXPECT_NE(log.find("warning bond_kept_unowned_enrolment id-"), std::string::npos) << log;
            EXPECT_EQ(log.find(primary_peer.address), std::string::npos) << "the raw address was logged";
        }
        if (GetParam() != WindowEnd::acknowledged_restart && GetParam() != WindowEnd::crash_restart)
            expect_refused_everywhere(backend, mock);
    }
    if (GetParam() == WindowEnd::crash_restart) {
        std::ofstream{directory + "/pending-removals", std::ios::trunc} << journal;
        ASSERT_TRUE(peer_in(directory, "pending-removals"));
    }
    for (int run = 0; run < 2; ++run) {
        // The owner unpairs the phone and pairs it again outside Android Auto: a replacement bond.
        ASSERT_EQ(mock.command("unpaired"), "ok");
        ASSERT_EQ(mock.command("paired"), "ok");
        helper::BluezBackend restarted{bus_address, clock, directory, ::getuid()};
        ASSERT_TRUE(restarted.apply(helper::Action::register_profile, 0));
        for (int attempt = 0; attempt < 5; ++attempt) restarted.tick();
        testing::internal::CaptureStderr();
        const auto reconciled = restarted.reconcile_bonds({});
        const auto log = testing::internal::GetCapturedStderr();
        ASSERT_TRUE(reconciled);
        EXPECT_EQ(log.find(primary_peer.address), std::string::npos) << "the raw address was logged";
        if (run == 0 && (GetParam() == WindowEnd::acknowledged_restart || GetParam() == WindowEnd::crash_restart)) {
            EXPECT_NE(log.find("warning bond_kept_unowned_enrolment id-"), std::string::npos) << log;
        }
        EXPECT_FALSE(peer_in(directory, "pending-removals")) << "the released entry is still journalled";
        EXPECT_FALSE(peer_in(directory, "managed-bonds")) << "a bond without a commit was admitted";
        EXPECT_FALSE(restarted.revoke_peer(primary_peer)) << "a revocation without authority reported success";
        *offset += 61s;
        for (int attempt = 0; attempt < 5; ++attempt) restarted.tick();
        expect_refused_everywhere(restarted, mock);
        if (HasFatalFailure()) return;
    }
    // A full enrolment is still admitted once the owner unpairs the phone.
    ASSERT_EQ(mock.command("unpaired"), "ok");
    helper::BluezBackend backend{bus_address, clock, directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({}));
    const auto fresh = backend.observe();
    ASSERT_TRUE(fresh);
    accept_enrolment(backend, mock, fresh.value().handle);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(admit(backend));
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    ASSERT_TRUE(backend.reconcile_bonds({primary_peer}));
    const auto reconnect = backend.observe();
    ASSERT_TRUE(reconnect);
    EXPECT_TRUE(backend.reconnect_peer(reconnect.value().handle)) << "the committed enrolment was not admitted";
}
INSTANTIATE_TEST_SUITE_P(EachEnd, WindowBond,
                         testing::Values(WindowEnd::cancel, WindowEnd::reject, WindowEnd::timeout,
                                         WindowEnd::acknowledged_restart, WindowEnd::crash_restart));
// A device continuity loss (BlueZ reports Connected=false) between the acknowledgement and the
// commit: the receiver's approval may already be durable, so the acknowledged bond is parked, its
// journal entry kept, and it stays refused until the receiver's reconciliation decides, in this
// helper run or after a restart. An approval admits it; an empty set releases it, the bond kept.
enum class AfterLoss { approved, restart_approved, restart_unapproved };
class ContinuityLoss : public ManagedBonds, public testing::WithParamInterface<AfterLoss> {};
TEST_P(ContinuityLoss, ParksAnAcknowledgedBondForTheReceiversAnswer) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    auto backend = std::make_unique<helper::BluezBackend>(bus_address, steady(), directory, ::getuid());
    ASSERT_TRUE(backend->apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend->reconcile_bonds({}));
    const auto obs = backend->observe();
    ASSERT_TRUE(obs);
    accept_enrolment(*backend, mock, obs.value().handle);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(backend->acknowledge_bond());
    // The receiver's trust write lands, then the link drops before its commit arrives.
    ASSERT_EQ(mock.command("connected-false"), "ok");
    for (int attempt = 0; attempt < 10; ++attempt) {
        backend->tick(); std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    EXPECT_FALSE(backend->commit_bond()) << "a commit after the continuity loss was accepted";
    EXPECT_TRUE(peer_in(directory, "pending-removals")) << "the continuity loss released an acknowledged bond";
    EXPECT_FALSE(peer_in(directory, "managed-bonds"));
    expect_refused_everywhere(*backend, mock);
    if (HasFatalFailure()) return;
    if (GetParam() != AfterLoss::approved) {
        backend.reset();
        backend = std::make_unique<helper::BluezBackend>(bus_address, steady(), directory, ::getuid());
        ASSERT_TRUE(backend->apply(helper::Action::register_profile, 0));
        EXPECT_TRUE(peer_in(directory, "pending-removals")) << "the restart lost the parked journal entry";
    }
    const bool approved = GetParam() != AfterLoss::restart_unapproved;
    const auto answer = approved ? backend->reconcile_bonds({primary_peer}) : backend->reconcile_bonds({});
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer.value(), 0U);
    EXPECT_FALSE(peer_in(directory, "pending-removals"));
    if (approved) {
        EXPECT_TRUE(peer_in(directory, "managed-bonds")) << "the durable approval was not recovered";
        const auto reconnect = backend->observe();
        ASSERT_TRUE(reconnect);
        EXPECT_TRUE(backend->reconnect_peer(reconnect.value().handle)) << "the recovered bond does not reconnect";
    } else {
        EXPECT_FALSE(peer_in(directory, "managed-bonds"));
        expect_refused_everywhere(*backend, mock);
    }
    EXPECT_EQ(state_number(mock, "destructive"), 0U);
}
INSTANTIATE_TEST_SUITE_P(EachAnswer, ContinuityLoss,
                         testing::Values(AfterLoss::approved, AfterLoss::restart_approved, AfterLoss::restart_unapproved));
// A device BlueZ holds in a temporary pairing (Paired, not Bonded) when an enrolment window for it
// closes: BlueZ turns CancelPairing without an in-flight Pair() into an unpair, so the helper never
// sends it (#136); the pending comparison is rejected through the agent's reply instead.
TEST_F(ManagedBonds, ClosingAWindowNeverCancelsATemporaryPairing) {
    using namespace std::chrono_literals;
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    ASSERT_EQ(mock.command("temporary-pair"), "ok");
    helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({}));
    const auto obs = backend.observe();
    ASSERT_TRUE(obs);
    ASSERT_TRUE(backend.authorize(obs.value().handle));
    ASSERT_EQ(mock.command("comparison-start"), "ok");
    std::optional<helper::PairingComparison> challenge;
    for (int attempt = 0; attempt < 150 && !challenge; ++attempt) {
        if (const auto value = backend.comparison()) challenge = value.value(); else std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(challenge.has_value());
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    EXPECT_EQ(mock.command("comparison-result"), "rejected");
    EXPECT_EQ(state_number(mock, "cancel_unpaired"), 0U) << "the helper unpaired a temporary pairing";
    EXPECT_NE(mock.command("state").find("\"paired\": true"), std::string::npos) << "the pairing was lost";
    EXPECT_EQ(state_number(mock, "destructive"), 0U);
    EXPECT_FALSE(peer_in(directory, "pending-removals"));
    const auto again = backend.observe();
    ASSERT_TRUE(again);
    EXPECT_FALSE(backend.reconnect_peer(again.value().handle));
}
// An unrelated bond (no helper file names it) is never touched by reconciliation.
TEST_F(ManagedBonds, ReconciliationNeverRemovesABondTheHelperDoesNotName) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    ASSERT_EQ(mock.command("paired"), "ok");
    helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({}));
    EXPECT_EQ(removals(mock), 0U);
}
// Process exit between each step of the demotion order (history, then managed). The child is this
// test binary, re-executed with the crash point in its environment; the trust test's fsync/renameat
// seam observes every step and exits the process at that point. No step owes or attempts a removal
// (#136).
enum class CrashPoint { history, managed };
TEST(DemotionCrashChild, ForgetsUntilTheCrashPoint) {
    const char* dir = std::getenv("AA_DEMOTION_CRASH_DIR");
    const char* point = std::getenv("AA_DEMOTION_CRASH_POINT");
    if (!dir || !point) GTEST_SKIP() << "run only as the crash test's child process";
    const std::string directory{dir};
    const auto crash = static_cast<CrashPoint>(std::atoi(point));
    helper::BluezBackend backend{std::getenv("DBUS_SESSION_BUS_ADDRESS"), steady(), directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({primary_peer}));
    auto seen = std::make_shared<int>(0);
    trust::test::faults::set_observer([directory, crash, seen] {
        const bool journal = peer_in(directory, "pending-removals"), history = peer_in(directory, "revoked-bonds"),
                   managed_entry = peer_in(directory, "managed-bonds");
        bool at = false;
        switch (crash) {
        case CrashPoint::history: at = history && managed_entry && ++*seen == 2; break;
        case CrashPoint::managed: at = !managed_entry && ++*seen == 1; break;
        }
        if (journal) ::_exit(8);  // a forget journalled a removal
        if (at) ::_exit(9);
    });
    (void)backend.revoke_peer(primary_peer);
    ::_exit(0);  // the crash point was never reached: the parent fails
}
class DemotionCrash : public ManagedBonds, public testing::WithParamInterface<int> {};
TEST_P(DemotionCrash, RestartAndReconciliationLeaveNoApprovalAndKeepTheBond) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    enrol_admitted(bus_address, mock, directory);
    if (HasFatalFailure()) return;
    const auto point = std::to_string(GetParam());
    const std::string env_dir = "AA_DEMOTION_CRASH_DIR=" + directory, env_point = "AA_DEMOTION_CRASH_POINT=" + point;
    std::vector<std::string> env;
    for (char** item = environ; *item; ++item) env.emplace_back(*item);
    env.push_back(env_dir);
    env.push_back(env_point);
    std::vector<char*> envp;
    for (auto& item : env) envp.push_back(item.data());
    envp.push_back(nullptr);
    std::string self = std::filesystem::read_symlink("/proc/self/exe").string();
    std::string filter = "--gtest_filter=DemotionCrashChild.ForgetsUntilTheCrashPoint";
    char* argv[]{self.data(), filter.data(), nullptr};
    pid_t child{};
    ASSERT_EQ(::posix_spawn(&child, self.c_str(), nullptr, nullptr, argv, envp.data()), 0);
    int status{};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 9) << "the helper never reached crash point " << point;
    EXPECT_EQ(removals(mock), 0U);
    expect_forgotten_and_kept(bus_address, mock, directory);
}
INSTANTIATE_TEST_SUITE_P(EachStep, DemotionCrash, testing::Range(0, 2));

// --- Revocation wins until a fresh enrolment commits (P2-2). ---
enum class Unaccepted { reject, cancel, timeout };
class RevokedApproval : public ManagedBonds, public testing::WithParamInterface<Unaccepted> {};
TEST_P(RevokedApproval, OnlyACommittedFreshEnrolmentClearsTheRevocation) {
    using namespace std::chrono_literals;
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    auto offset = std::make_shared<std::chrono::steady_clock::duration>(0s);
    helper::BluezBackend backend{bus_address, [offset] { return std::chrono::steady_clock::now() + *offset; },
                                 directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({}));
    const auto first = backend.observe();
    ASSERT_TRUE(first);
    accept_enrolment(backend, mock, first.value().handle);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(admit(backend));
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    ASSERT_TRUE(backend.revoke_peer(primary_peer));  // ForgetPhone
    ASSERT_EQ(removals(mock), 0U) << "a forgotten bond was deleted outside an enrolment window";
    ASSERT_TRUE(peer_in(directory, "revoked-bonds"));
    // The receiver's forget was lost (power loss after a non-durable write): the approval is back.
    ASSERT_EQ(mock.command("restore"), "ok");
    const auto resurrected = backend.reconcile_bonds({primary_peer});
    ASSERT_TRUE(resurrected);
    EXPECT_EQ(resurrected.value(), 1U);
    // An enrolment opens for the identity and a bond forms, but no number is ever accepted.
    const auto again = backend.observe();
    ASSERT_TRUE(again);
    ASSERT_TRUE(backend.authorize(again.value().handle));
    EXPECT_TRUE(peer_in(directory, "revoked-bonds")) << "opening an enrolment cleared the revocation";
    ASSERT_EQ(mock.command("comparison-start"), "ok");
    std::optional<helper::PairingComparison> challenge;
    for (int attempt = 0; attempt < 150 && !challenge; ++attempt) {
        if (const auto value = backend.comparison()) challenge = value.value(); else std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(challenge.has_value());
    ASSERT_EQ(mock.command("paired"), "ok");
    switch (GetParam()) {
    case Unaccepted::reject: ASSERT_TRUE(backend.respond_comparison({challenge->id, false})); break;
    case Unaccepted::cancel: ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0)); break;
    case Unaccepted::timeout:
        *offset = 61s;
        for (int attempt = 0; attempt < 5; ++attempt) { backend.tick(); std::this_thread::sleep_for(20ms); }
        break;
    }
    EXPECT_TRUE(peer_in(directory, "revoked-bonds")) << "an enrolment without acceptance cleared the revocation";
    const auto answer = backend.reconcile_bonds({primary_peer});
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer.value(), 1U) << "the refused approval was not surfaced";
    EXPECT_FALSE(peer_in(directory, "managed-bonds")) << "reconciliation admitted an unaccepted bond";
    const auto refused = backend.observe();
    ASSERT_TRUE(refused);
    EXPECT_FALSE(backend.reconnect_peer(refused.value().handle)) << "reconnect of a revoked identity succeeded";
    // A full fresh enrolment: number accepted, bond acknowledged, the receiver's commit.
    ASSERT_EQ(mock.command("unpaired"), "ok");
    const auto fresh = backend.observe();
    ASSERT_TRUE(fresh);
    accept_enrolment(backend, mock, fresh.value().handle);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(admit(backend));
    EXPECT_FALSE(peer_in(directory, "revoked-bonds")) << "the committed enrolment kept the revocation";
    EXPECT_TRUE(peer_in(directory, "managed-bonds"));
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    const auto settled = backend.reconcile_bonds({primary_peer});
    ASSERT_TRUE(settled);
    EXPECT_EQ(settled.value(), 0U);
    const auto reconnect = backend.observe();
    ASSERT_TRUE(reconnect);
    EXPECT_TRUE(backend.reconnect_peer(reconnect.value().handle)) << "the committed enrolment was not admitted";
}
INSTANTIATE_TEST_SUITE_P(WithoutAcceptance, RevokedApproval,
                         testing::Values(Unaccepted::reject, Unaccepted::cancel, Unaccepted::timeout));
// Commit crash window 1 for a re-enrolled (revoked) identity within one helper run: the fresh
// enrolment was accepted and acknowledged, the commit never arrived, the approval landed.
TEST_F(ManagedBonds, AcknowledgedReEnrolmentOfARevokedIdentityIsAdmittedOnReconciliation) {
    const auto* bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(bus_address, nullptr);
    MockBluez mock{bus_address};
    helper::BluezBackend backend{bus_address, steady(), directory, ::getuid()};
    ASSERT_TRUE(backend.apply(helper::Action::register_profile, 0));
    ASSERT_TRUE(backend.reconcile_bonds({}));
    const auto first = backend.observe();
    ASSERT_TRUE(first);
    accept_enrolment(backend, mock, first.value().handle);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(admit(backend));
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));
    ASSERT_TRUE(backend.revoke_peer(primary_peer));
    ASSERT_EQ(mock.command("restore"), "ok");
    const auto again = backend.observe();
    ASSERT_TRUE(again);
    accept_enrolment(backend, mock, again.value().handle);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(backend.acknowledge_bond());
    ASSERT_TRUE(backend.apply(helper::Action::close_pairing, 0));  // the receiver exits before its commit
    EXPECT_TRUE(peer_in(directory, "revoked-bonds"));
    const auto answer = backend.reconcile_bonds({primary_peer});
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer.value(), 0U);
    EXPECT_FALSE(peer_in(directory, "revoked-bonds"));
    EXPECT_TRUE(peer_in(directory, "managed-bonds"));
    EXPECT_EQ(removals(mock), 0U) << "the forget or the committed re-enrolment deleted a bond";
    const auto obs = backend.observe();
    ASSERT_TRUE(obs);
    EXPECT_TRUE(backend.reconnect_peer(obs.value().handle));
}

TEST(HelperOwner, NonRootOwnerIsRefusedBeforeAnyHelperCall) {
    if (::getuid() == 0) GTEST_SKIP() << "requires non-root bus owner";
    using namespace ipc::dbus_adapter;
    auto service = BusConnection::connect_session(); ASSERT_TRUE(service);
    ASSERT_TRUE(service.value().request_name(ipc::BusName{helper::kBusName}));
    auto pattern = (std::filesystem::path{AA_HELPER_BINARY_DIR} / "aa-owner-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    trust::ApprovedPhoneStore store{std::filesystem::path{pattern} / "phones"}; ASSERT_TRUE(store.load());
    auto client = helper::BusClient::private_bus(std::getenv("DBUS_SESSION_BUS_ADDRESS"), store); ASSERT_TRUE(client);
    const auto result = client.value()->status(helper::Action::ap_status);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::helper_rejected);
    std::filesystem::remove_all(pattern);
}
}
