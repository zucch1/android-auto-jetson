// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/Identity.hpp>
#include "../../src/helper/crypto_adapter/Crypto.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::optional<struct stat> coalesced;
struct Child final {
    pid_t pid{};
    int input{-1}, output{-1};
    ~Child() {
        if (pid > 0) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
        if (input >= 0) ::close(input);
        if (output >= 0) ::close(output);
        coalesced.reset();
    }
    bool ready() const {
        ::pollfd event{output, POLLIN, 0};
        char value{};
        return ::poll(&event, 1, 1000) == 1 && ::read(output, &value, 1) == 1 && value == 'r';
    }
    bool advance() const { return ::write(input, "x", 1) == 1 && ready(); }
};
}
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __wrap_fstat(int fd, struct stat* value) {
    const auto result = __real_fstat(fd, value);
    if (result == 0 && coalesced && value->st_dev == coalesced->st_dev && value->st_ino == coalesced->st_ino) {
        value->st_mtim = coalesced->st_mtim;
        value->st_ctim = coalesced->st_ctim;
    }
    return result;
}
TEST(HelperIdentityReexec, RejectsChangedSameInodeAfterExecAwayAndBackWithinOperation) {
    // Given an executing copy and a filesystem whose timestamps coalesce writes.
    char name[] = "/tmp/aa-identity-reexec-XXXXXX";
    const char* temporary = ::mkdtemp(name); ASSERT_NE(temporary, nullptr);
    struct Directory {
        std::filesystem::path path;
        ~Directory() { std::filesystem::remove_all(path); }
    } directory{temporary};
    const auto image = directory.path / "image";
    const std::string original{AA_IDENTITY_EXEC_FIXTURE};
    std::filesystem::copy_file(original, image);
    { std::ofstream output{image, std::ios::binary | std::ios::app}; output << 'a'; }
    int to_child[2]{}, from_child[2]{};
    ASSERT_EQ(::pipe(to_child), 0); ASSERT_EQ(::pipe(from_child), 0);
    Child child;
    child.pid = ::fork(); ASSERT_GE(child.pid, 0);
    if (child.pid == 0) {
        ::dup2(to_child[0], STDIN_FILENO); ::dup2(from_child[1], STDOUT_FILENO);
        ::close(to_child[0]); ::close(to_child[1]); ::close(from_child[0]); ::close(from_child[1]);
        ::execl(image.c_str(), image.c_str(), image.c_str(), original.c_str(), "0", static_cast<char*>(nullptr));
        ::_exit(8);
    }
    ::close(to_child[0]); ::close(from_child[1]); child.input = to_child[1]; child.output = from_child[0];
    ASSERT_TRUE(child.ready());
    const int fd = ::open(image.c_str(), O_RDONLY | O_CLOEXEC); ASSERT_GE(fd, 0);
    struct stat metadata{}; ASSERT_EQ(__real_fstat(fd, &metadata), 0);
    const auto digest = aa::helper::crypto_adapter::hash_executable(fd,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500));
    ::close(fd); ASSERT_TRUE(digest);
    coalesced = metadata;
    const aa::helper::IdentityPolicy policy{::getuid(), digest.value()};
    const auto resolve = [&] {
        return aa::helper::inspect_process(":1.3", ::getuid(), static_cast<std::uint32_t>(child.pid), policy,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500));
    };
    ASSERT_TRUE(resolve());
    // When it execs away, rewrites the same inode without changing size, then returns.
    ASSERT_TRUE(child.advance());
    { std::fstream output{image, std::ios::binary | std::ios::in | std::ios::out}; output.seekp(-1, std::ios::end); output << 'b'; }
    ASSERT_TRUE(child.advance());
    // Then the second resolution must hash the modified bytes, not reuse metadata.
    EXPECT_FALSE(resolve());
}
