// SPDX-License-Identifier: GPL-3.0-or-later
// Gate round-2 ownership regressions (task 22): descriptor and endpoint
// release across move assignment (F1) and endpoint-path ownership rules (F2).
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

constexpr int kTimeoutMs = 2000;

bool node_exists(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

int open_fd_count() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count - 2;
}

bool leave_stale_socket_node(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return bound;
}

bool write_file(const std::string& path, const char* body) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t size = std::strlen(body);
    const bool written = std::fwrite(body, 1, size, file) == size;
    std::fclose(file);
    return written;
}

bool streams_one_unit(VideoServer& server, const VideoSocketPath& path) {
    aa::core::Result<VideoConsumer> client{VideoConsumer{}};
    std::thread connector([&] {
        client = connect_video_consumer(
            path, VideoClientOptions{{}, current_uid(), aa::core::Milliseconds{kTimeoutMs}});
    });
    auto producer = server.accept_consumer(aa::core::Milliseconds{kTimeoutMs});
    connector.join();
    if (!producer || !client) {
        return false;
    }
    const std::vector<std::uint8_t> payload{0x5a};
    if (!producer.value().send_access_unit({1, 1, true, payload})) {
        return false;
    }
    const auto step = client.value().receive(aa::core::Milliseconds{kTimeoutMs});
    return step && step.value() == ReceiveStatus::complete;
}

std::string directory_of(const VideoSocketPath& path) {
    return path.value.substr(0, path.value.find_last_of('/'));
}

void cleanup_runtime(const std::string& runtime, const std::string& leaf) {
    ::unlink(leaf.c_str());
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

#if defined(__x86_64__)
constexpr unsigned kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr unsigned kAuditArch = AUDIT_ARCH_AARCH64;
#else
#error "video ownership tests need a known audit architecture for the seccomp filter"
#endif

// The chmod family is the only thing denied; everything else keeps running so
// listen() reaches its own chmod and must fail closed. Child exit codes name
// the failing step: 1 filter inert, 2 listen unexpectedly succeeded, 3 wrong
// error code, 4 endpoint node left behind, 5 descriptor leaked.
void deny_chmod_syscalls() {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned>(offsetof(struct seccomp_data, arch))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned>(offsetof(struct seccomp_data, nr))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fchmod, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fchmodat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#ifdef __NR_fchmodat2
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fchmodat2, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_chmod
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_chmod, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program {
        .len = static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return;
    }
    (void)::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program);
}

int chmod_failure_child(const std::string& runtime) {
    deny_chmod_syscalls();
    errno = 0;
    if (::chmod(runtime.c_str(), 0700) == 0 || errno != EPERM) {
        return 1;
    }
    const auto path = video_socket_path_in(runtime);
    const int before = open_fd_count();
    auto server = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    if (server) {
        return 2;
    }
    if (server.error().code() != aa::ErrorCode::transport_io) {
        return 3;
    }
    if (node_exists(path.value)) {
        return 4;
    }
    return open_fd_count() == before ? 0 : 5;
}
} // namespace

TEST(VideoOwnership, MoveAssignProducerReleasesDescriptorAndNotifiesPeer) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& producer = pair.value().producer;
    const int old_fd = producer.native_handle();
    ASSERT_GE(old_fd, 0);

    VideoProducer& alias = producer;
    producer = std::move(alias);
    EXPECT_FALSE(producer.closed());
    EXPECT_NE(::fcntl(old_fd, F_GETFD), -1);

    VideoProducer donor{};
    VideoProducer holder{std::move(donor)};
    EXPECT_TRUE(holder.closed());
    producer = std::move(donor);

    errno = 0;
    const int probed = ::fcntl(old_fd, F_GETFD);
    const int failure = errno;
    EXPECT_EQ(probed, -1);
    EXPECT_EQ(failure, EBADF);
    EXPECT_TRUE(producer.closed());
    const auto eof = pair.value().consumer.receive(aa::core::Milliseconds{kTimeoutMs});
    ASSERT_FALSE(eof);
    EXPECT_EQ(eof.error().code(), aa::ErrorCode::transport_closed);
}

TEST(VideoOwnership, MoveAssignConsumerReleasesDescriptorAndNotifiesPeer) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& consumer = pair.value().consumer;
    const int old_fd = consumer.native_handle();
    ASSERT_GE(old_fd, 0);

    VideoConsumer donor{};
    VideoConsumer holder{std::move(donor)};
    EXPECT_TRUE(holder.closed());
    consumer = std::move(donor);

    errno = 0;
    const int probed = ::fcntl(old_fd, F_GETFD);
    const int failure = errno;
    EXPECT_EQ(probed, -1);
    EXPECT_EQ(failure, EBADF);
    EXPECT_TRUE(consumer.closed());
    const std::vector<std::uint8_t> payload{0x5a};
    const auto send = pair.value().producer.send_access_unit({1, 1, true, payload});
    ASSERT_FALSE(send);
    EXPECT_EQ(send.error().code(), aa::ErrorCode::transport_io);
}

TEST(VideoOwnership, MoveAssignServerReleasesDescriptorAndOwnedEndpoint) {
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    ASSERT_TRUE(server);
    const int old_fd = server.value().native_handle();
    ASSERT_GE(old_fd, 0);
    ASSERT_TRUE(node_exists(path.value));

    VideoServer donor{};
    VideoServer holder{std::move(donor)};
    EXPECT_TRUE(holder.closed());
    server.value() = std::move(donor);

    errno = 0;
    const int probed = ::fcntl(old_fd, F_GETFD);
    const int failure = errno;
    EXPECT_EQ(probed, -1);
    EXPECT_EQ(failure, EBADF);
    EXPECT_FALSE(node_exists(path.value));
    EXPECT_TRUE(server.value().closed());
    cleanup_runtime(runtime, path.value);
}

TEST(VideoOwnership, PrepareRefusesLiveEndpointAndLeavesItIntact) {
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    ASSERT_TRUE(server);

    const auto refused = prepare_video_socket_path(path);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unavailable);
    EXPECT_TRUE(node_exists(path.value));
    EXPECT_TRUE(streams_one_unit(server.value(), path));

    server.value().close();
    cleanup_runtime(runtime, path.value);
}

TEST(VideoOwnership, PrepareRejectsRegularFileWithoutDeletingIt) {
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    ASSERT_EQ(::mkdir(directory_of(path).c_str(), 0700), 0);
    ASSERT_TRUE(write_file(path.value, "keep-me"));

    const auto refused = prepare_video_socket_path(path);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unavailable);
    struct stat info {};
    ASSERT_EQ(::lstat(path.value.c_str(), &info), 0);
    EXPECT_TRUE(S_ISREG(info.st_mode));
    EXPECT_EQ(info.st_size, 7);
    const auto listen_refused = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    ASSERT_FALSE(listen_refused);
    EXPECT_EQ(listen_refused.error().code(), aa::ErrorCode::transport_unavailable);
    EXPECT_TRUE(node_exists(path.value));

    cleanup_runtime(runtime, path.value);
}

TEST(VideoOwnership, PrepareRejectsSymlinkWithoutFollowingIt) {
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    ASSERT_EQ(::mkdir(directory_of(path).c_str(), 0700), 0);
    const std::string victim = runtime + "/victim.bin";
    ASSERT_TRUE(write_file(victim, "victim-content"));
    ASSERT_EQ(::symlink(victim.c_str(), path.value.c_str()), 0);

    const auto refused = prepare_video_socket_path(path);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unavailable);
    struct stat info {};
    ASSERT_EQ(::lstat(path.value.c_str(), &info), 0);
    EXPECT_TRUE(S_ISLNK(info.st_mode));
    ASSERT_EQ(::stat(victim.c_str(), &info), 0);
    EXPECT_EQ(info.st_size, 14);

    ::unlink(victim.c_str());
    cleanup_runtime(runtime, path.value);
}

TEST(VideoOwnership, ServerTeardownKeepsReplacementEndpoint) {
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto first = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    ASSERT_TRUE(first);
    ASSERT_EQ(::unlink(path.value.c_str()), 0);
    auto second = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    ASSERT_TRUE(second);

    first.value().close();
    EXPECT_TRUE(node_exists(path.value));
    EXPECT_TRUE(streams_one_unit(second.value(), path));

    second.value().close();
    EXPECT_FALSE(node_exists(path.value));
    cleanup_runtime(runtime, path.value);
}

TEST(VideoOwnership, PrepareRemovesVerifiedStaleSocketNode) {
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    ASSERT_EQ(::mkdir(directory_of(path).c_str(), 0700), 0);
    ASSERT_TRUE(leave_stale_socket_node(path.value));
    ASSERT_TRUE(node_exists(path.value));

    const auto prepared = prepare_video_socket_path(path);
    ASSERT_TRUE(prepared);
    EXPECT_FALSE(node_exists(path.value));
    auto server = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    ASSERT_TRUE(server);
    EXPECT_TRUE(streams_one_unit(server.value(), path));

    server.value().close();
    cleanup_runtime(runtime, path.value);
}

TEST(VideoOwnership, ChmodFailureFailsClosed) {
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        ::_exit(chmod_failure_child(runtime));
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "chmod failure child step " << WEXITSTATUS(status);
    cleanup_runtime(runtime, video_socket_path_in(runtime).value);
}
