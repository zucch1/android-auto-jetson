// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/Identity.hpp>
#include "../../src/ipc/dbus_adapter/BusIo.hpp"
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace aa;
using namespace aa::helper;
std::string self_digest() {
    std::ifstream input{"/proc/self/exe", std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
    unsigned char digest[32]{};
    std::size_t length{};
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, bytes.data(), bytes.size(), digest, &length) != 1 || length != 32)
        throw std::runtime_error("digest failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto c : digest) { result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    return result;
}
class HelperIdentity : public ::testing::Test {
protected:
    std::filesystem::path directory;
    void SetUp() override {
        directory = std::filesystem::current_path() / ("identity-" + std::to_string(::getpid()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        std::ofstream{directory / "session.uid"} << ::getuid() << '\n';
        std::ofstream{directory / "receiver.sha256"} << self_digest() << '\n';
        for (const auto* name : {"session.uid", "receiver.sha256"})
            std::filesystem::permissions(directory / name, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
    core::Result<IdentityPolicy> load(std::uint32_t owner) {
        return IdentityPolicy::load((directory / "session.uid").string(),
                                    (directory / "receiver.sha256").string(), owner);
    }
};
TEST_F(HelperIdentity, FollowsKernelExeLeafAndChecksUidDigestAndDeath) {
    auto policy = load(::getuid()); ASSERT_TRUE(policy);
    const auto identity = inspect_process(":1.3", ::getuid(), ::getpid(), policy.value());
    ASSERT_TRUE(identity);
    EXPECT_GT(identity.value().starttime, 0U);
    EXPECT_EQ(identity.value().digest, self_digest());
    EXPECT_FALSE(inspect_process(":1.3", ::getuid() + 1, ::getpid(), policy.value()));
    policy.value().digest[0] = policy.value().digest[0] == 'a' ? 'b' : 'a';
    EXPECT_FALSE(inspect_process(":1.3", ::getuid(), ::getpid(), policy.value()));
    EXPECT_FALSE(inspect_process(":1.3", ::getuid(), 0, policy.value()));
    EXPECT_FALSE(inspect_process("org.forged.Receiver", ::getuid(), ::getpid(), policy.value()));
}
TEST_F(HelperIdentity, PolicyRejectsWrongOwnerWritableMalformedAndSymlinkFiles) {
    if (::getuid() != 0) EXPECT_FALSE(load(0));
    std::ofstream{directory / "receiver.sha256"} << std::string(64, 'a') << "  /arbitrary/path\n";
    EXPECT_FALSE(load(::getuid()));
    std::ofstream{directory / "receiver.sha256"} << std::string(64, 'A') << '\n';
    EXPECT_FALSE(load(::getuid()));
    std::ofstream{directory / "receiver.sha256"} << self_digest() << '\n';
    std::filesystem::permissions(directory / "receiver.sha256", std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::add);
    EXPECT_FALSE(load(::getuid()));
    std::filesystem::permissions(directory / "receiver.sha256", std::filesystem::perms::owner_read);
    std::filesystem::rename(directory / "receiver.sha256", directory / "real-digest");
    std::filesystem::create_symlink(directory / "real-digest", directory / "receiver.sha256");
    EXPECT_FALSE(load(::getuid()));
}
TEST_F(HelperIdentity, RehashRejectsExecAndDeadProcessWithUnchangedPid) {
    const auto policy = load(::getuid()); ASSERT_TRUE(policy);
    int trigger[2]{};
    ASSERT_EQ(::pipe(trigger), 0);
    const auto pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::close(trigger[1]);
        char byte{};
        if (::read(trigger[0], &byte, 1) != 1) ::_exit(2);
        ::close(trigger[0]);
        ::execl("/usr/bin/sleep", "sleep", "2", static_cast<char*>(nullptr));
        ::_exit(3);
    }
    struct Child {
        pid_t pid;
        int fd;
        ~Child() { ::close(fd); ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
    } child{pid, trigger[1]};
    ::close(trigger[0]);
    const auto before = inspect_process(":1.3", ::getuid(), static_cast<std::uint32_t>(pid), policy.value());
    ASSERT_TRUE(before);
    ASSERT_EQ(::write(trigger[1], "x", 1), 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool rejected = false;
    while (!rejected && std::chrono::steady_clock::now() < until)
        rejected = !inspect_process(":1.3", ::getuid(), static_cast<std::uint32_t>(pid), policy.value());
    EXPECT_TRUE(rejected);
    EXPECT_EQ(::waitpid(pid, nullptr, WNOHANG), 0);
    ASSERT_EQ(::kill(pid, SIGKILL), 0);
    ASSERT_EQ(::waitpid(pid, nullptr, 0), pid);
    EXPECT_FALSE(inspect_process(":1.3", ::getuid(), static_cast<std::uint32_t>(pid), policy.value()));
}
TEST(HelperDeadline, TrickleReadsShareOneAbsoluteDeadline) {
    int sockets[2]{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    std::thread writer{[fd = sockets[1]] {
        const char byte = 'a';
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            (void)::send(fd, &byte, 1, MSG_NOSIGNAL);
        }
        ::close(fd);
    }};
    const auto start = std::chrono::steady_clock::now();
    std::array<std::uint8_t, 10> data{};
    EXPECT_FALSE(ipc::dbus_adapter::bus_io::read_full(sockets[0], data, start + std::chrono::milliseconds(100)));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(250));
    ::close(sockets[0]);
    writer.join();
}
}
