// SPDX-License-Identifier: GPL-3.0-or-later
#include "PendingIo.hpp"
#include <aa/core/Cancellation.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/TlsTransport.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

namespace tr = aa::transport;

bool handshake(tr::TlsTransport& client, tr::TlsTransport& server) {
    bool client_ok = false;
    bool server_ok = false;
    std::thread client_thread([&] { client_ok = client.open({}).has_value(); });
    std::thread server_thread([&] { server_ok = server.open({}).has_value(); });
    client_thread.join();
    server_thread.join();
    return client_ok && server_ok;
}

std::vector<std::byte> frame_of(std::size_t size) {
    return tr::encode_frame({1, tr::FrameType::bulk, tr::MessageKind::specific,
                             tr::Encryption::plain},
                            std::vector<std::byte>(size, std::byte{7}), 0)
        .value();
}

} // namespace

TEST(TlsLifetime, StalledHandshakeCancelsWithinDeadline) {
    // Given: a client whose server never begins its half of the handshake.
    auto pair = tr::make_tls_loopback([] { return true; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair);
    aa::core::CancellationSource source;
    PendingIo pending([&] { return pair.value().client->open(source.get_token()); });
    const bool blocked = pending.blocked(SYS_poll);
    // When: cancellation fires while the handshake waits for server bytes.
    source.request_stop();
    // Then: it returns cancelled promptly, well inside the 10s whole-handshake
    // deadline, and the transport is closed.
    EXPECT_TRUE(blocked);
    ASSERT_EQ(pending.result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto opened = pending.result.get();
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), aa::ErrorCode::cancelled);
    auto after = pair.value().client->receive();
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code(), aa::ErrorCode::transport_closed);
}

TEST(TlsLifetime, RepeatedCloseAndFdReuseDoesNoSslIo) {
    // Given: a handshaked transport whose exact socket fd is identified before
    // close via TCP getpeername (its peer is the known listener port) and /proc.
    auto listener = tr::TlsListener::listen("127.0.0.1", 0, [] { return true; },
                                            tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(listener);
    const auto port = listener.value()->port();
    ASSERT_NE(port, 0);
    auto client = tr::TlsTransport::connect("127.0.0.1", port, [] { return true; },
                                            tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(client);
    auto server = listener.value()->accept();
    ASSERT_TRUE(server);
    listener.value()->close();
    ASSERT_TRUE(handshake(*client.value(), *server.value()));

    const auto fd_identity = [](int fd) -> std::string {
        char target[64]{};
        const ssize_t size = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), target,
                                        sizeof(target) - 1);
        return size > 0 ? std::string(target, static_cast<std::size_t>(size)) : std::string{};
    };
    int transport_fd = -1;
    for (int fd = 0; fd < 1024; ++fd) {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
            address.sin_family == AF_INET && ntohs(address.sin_port) == port) {
            transport_fd = fd;
            break;
        }
    }
    ASSERT_GE(transport_fd, 0) << "client socket not identified by getpeername";
    const std::string socket_identity = fd_identity(transport_fd);
    EXPECT_EQ(socket_identity.substr(0, 7), "socket:") << "fd " << transport_fd;

    // The unrelated descriptor exists before the close so it cannot be handed
    // the number this test is about to reuse.
    std::array<int, 2> pipe_fds{};
    ASSERT_EQ(::pipe(pipe_fds.data()), 0);

    client.value()->close();
    client.value()->close();
    // The transport released exactly the identified fd: the destructor's stale
    // close would hit this same number.
    errno = 0;
    ASSERT_LT(::fcntl(transport_fd, F_GETFD), 0) << "close did not release the identified fd";
    ASSERT_EQ(errno, EBADF);

    // When: dup2 forces the pipe write end into that exact fd number, and the
    // source end is closed so the reused fd is the sole, known-owned write end.
    ASSERT_EQ(::dup2(pipe_fds[1], transport_fd), transport_fd);
    ASSERT_EQ(::close(pipe_fds[1]), 0);
    const std::array<std::byte, 1> sent{std::byte{0x5A}};
    ASSERT_EQ(::write(transport_fd, sent.data(), sent.size()), 1);
    std::array<std::byte, 1> received{};
    ASSERT_EQ(::read(pipe_fds[0], received.data(), received.size()), 1);
    EXPECT_EQ(received[0], sent[0]);
    const std::string reused_identity = fd_identity(transport_fd);
    EXPECT_EQ(reused_identity.substr(0, 5), "pipe:");

    // Then: destruction neither closes nor alters the reused descriptor and
    // performs no SSL I/O against it (no stray bytes appear in the pipe).
    client.value().reset();
    EXPECT_GE(::fcntl(transport_fd, F_GETFD), 0) << "destructor closed the reused fd";
    EXPECT_EQ(fd_identity(transport_fd), reused_identity) << "destructor altered the reused fd";
    ::pollfd stray{pipe_fds[0], POLLIN, 0};
    EXPECT_EQ(::poll(&stray, 1, 0), 0) << "destructor wrote to the reused fd";
    const std::array<std::byte, 1> sentinel{std::byte{0xA5}};
    ASSERT_EQ(::write(transport_fd, sentinel.data(), sentinel.size()), 1);
    ASSERT_EQ(::read(pipe_fds[0], received.data(), received.size()), 1);
    EXPECT_EQ(received[0], sentinel[0]);
    ::close(transport_fd);
    ::close(pipe_fds[0]);
}

TEST(TlsLifetime, DefaultSigpipeChildSurvivesClosedPeer) {
    // Given: a handshaked client whose peer has closed, and a child process with
    // the default SIGPIPE disposition and an UNBLOCKED SIGPIPE mask. The parent
    // blocks SIGPIPE before fork, so the child inherits a blocked mask and its
    // own unblock is load-bearing: without it a SIGPIPE would stay pending and
    // the proof would be vacuous.
    auto pair = tr::make_tls_loopback([] { return true; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair);
    auto client = std::move(pair.value().client);
    ASSERT_TRUE(handshake(*client, *pair.value().server));
    pair.value().server->close();
    sigset_t sigpipe{};
    ::sigemptyset(&sigpipe);
    ::sigaddset(&sigpipe, SIGPIPE);
    sigset_t previous_mask{};
    const int parent_block = ::sigprocmask(SIG_BLOCK, &sigpipe, &previous_mask);
    const pid_t child = ::fork();
    if (child == 0) {
        ::signal(SIGPIPE, SIG_DFL);
        // Unblock the inherited blocked mask and assert success in the child:
        // exit code 2 marks a failed sigprocmask, and the parent asserts exit 0.
        sigset_t unblock{};
        ::sigemptyset(&unblock);
        ::sigaddset(&unblock, SIGPIPE);
        if (::sigprocmask(SIG_UNBLOCK, &unblock, nullptr) != 0) {
            ::_exit(2);
        }
        const auto frame = frame_of(256);
        bool typed_error = false;
        for (int i = 0; i < 4096 && !typed_error; ++i) {
            auto sent = client->send(frame);
            typed_error = !sent && aa::domain_of(sent.error().code()) == aa::ErrorDomain::transport;
        }
        ::_exit(typed_error ? 0 : 1);
    }
    int status = 0;
    const pid_t waited = child > 0 ? ::waitpid(child, &status, 0) : -1;
    const int parent_restore = ::sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    ASSERT_EQ(parent_block, 0);
    ASSERT_GT(child, 0);
    ASSERT_EQ(waited, child);
    ASSERT_EQ(parent_restore, 0);
    // Then: the child exited normally on a typed error (never killed by SIGPIPE)
    // and its SIGPIPE unblock succeeded (2 would mean sigprocmask failed).
    ASSERT_TRUE(WIFEXITED(status)) << "child died by signal " << WTERMSIG(status);
    EXPECT_EQ(WEXITSTATUS(status), 0) << "2 means the child's SIGPIPE unblock failed";
}

TEST(TlsLifetime, AdmissionRejectionIsTerminalAndCloses) {
    // Given: a transport whose approved-phone predicate rejects the peer.
    auto pair = tr::make_tls_loopback([] { return false; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair);
    // When: open is attempted.
    auto opened = pair.value().client->open({});
    // Then: it fails closed and the rejected transport is terminal.
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    auto received = pair.value().client->receive();
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code(), aa::ErrorCode::transport_closed);
    auto sent = pair.value().client->send(frame_of(8));
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_closed);
}
