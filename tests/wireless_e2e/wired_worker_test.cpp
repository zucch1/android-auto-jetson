// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wired-receiver/VideoOutlet.hpp"
#include "../../tools/wireless-receiver/ManagedVideo.hpp"
#include "../../tools/wireless-receiver/WiredWorker.hpp"

#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/VideoChannel.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

namespace wr = aa::wireless_receiver;
namespace wired_rx = aa::wired_receiver;
using namespace std::chrono_literals;

struct Scratch final {
    Scratch() {
        char name[] = "/tmp/aa-wired-worker-XXXXXX";
        if (const auto* directory = ::mkdtemp(name)) path = directory;
    }
    ~Scratch() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    std::filesystem::path path;
};

std::vector<std::byte> to_bytes(std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const auto v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

aa::core::Result<aa::ipc::ReceiveStatus> receive_complete(aa::ipc::VideoConsumer& consumer,
                                                          int max_steps = 64) {
    for (int i = 0; i < max_steps; ++i) {
        auto step = consumer.receive(aa::core::Milliseconds{2000});
        if (!step) return step;
        if (step.value() == aa::ipc::ReceiveStatus::complete) return step;
    }
    return aa::Error{aa::ErrorCode::timeout};
}

// Bounded helper that connects a VideoConsumer while polling ManagedVideoBroker.
// Exits promptly with the connector's error if admission fails instead of spinning.
aa::core::Result<aa::ipc::VideoConsumer> connect_broker_consumer(
    const aa::ipc::VideoSocketPath& video_path, aa::ipc::VideoServer& server,
    wr::ManagedVideoBroker& broker,
    aa::ipc::NegotiatedLimits client_limits = {},
    std::chrono::milliseconds limit = 5s) {
    const auto before = broker.generation();
    aa::core::Result<aa::ipc::VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::atomic<bool> connector_done{false};
    std::thread connector([&] {
        consumer = aa::ipc::connect_video_consumer(
            video_path, {client_limits, aa::ipc::current_uid(), aa::core::Milliseconds{2000}});
        connector_done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (broker.generation() == before && !connector_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        if (!broker.has_pending_handshake(server)) {
            pollfd pfd{server.native_handle(), POLLIN, 0};
            (void)::poll(&pfd, 1, 20);
        } else {
            std::this_thread::yield();
        }
        (void)broker.poll(server);
    }
    if (broker.generation() == before) {
        (void)broker.poll(server);
    }
    connector.join();
    if (!consumer) return consumer;
    if (broker.generation() == before) return aa::Error{aa::ErrorCode::timeout};
    return consumer;
}

std::vector<std::byte> make_fragmented_idr(std::size_t total_bytes, std::uint8_t seed) {
    std::vector<std::byte> out(total_bytes);
    out[0] = std::byte{0x00};
    out[1] = std::byte{0x00};
    out[2] = std::byte{0x00};
    out[3] = std::byte{0x01};
    out[4] = std::byte{0x65};
    for (std::size_t i = 5; i < total_bytes; ++i) {
        out[i] = static_cast<std::byte>((i * 31U + seed) & 0xFFU);
    }
    return out;
}

TEST(ManagedWiredWorker, FindsApprovedSerialOnAnotherPortAndRemembersAccessoryLocation) {
    Scratch scratch;
    ASSERT_FALSE(scratch.path.empty());
    const auto device = scratch.path / "1-2.3";
    ASSERT_TRUE(std::filesystem::create_directory(device));
    const auto attribute = [&](const char* name, const char* value) {
        std::ofstream{device / name} << value << '\n';
    };
    attribute("idVendor", "18d1");
    attribute("idProduct", "4000");
    attribute("serial", "approved-native-serial");
    const aa::trust::WiredIdentity identity{
        "18d1", "4000", "approved-native-serial", "Android", "Android Auto",
        "approved-accessory-serial", "1-1"};
    const std::vector<aa::trust::ApprovedPhone> approved{
        {{17}, identity, aa::trust::IdentityStrength::serial_backed}};

    const auto found = wr::find_wired_candidate(approved, {}, scratch.path);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->port, "1-2.3");
    EXPECT_EQ(found->phone.value, 17U);
    EXPECT_FALSE(wr::find_wired_candidate({}, {}, scratch.path));

    attribute("idProduct", "2d01");
    attribute("manufacturer", "Android");
    attribute("product", "Android Auto");
    attribute("serial", "approved-accessory-serial");
    // A unique accessory identity is found on any port, such as after a
    // supervisor restart left the phone in accessory mode (#240 review P3-6).
    EXPECT_TRUE(wr::find_wired_candidate(approved, {}, scratch.path));
    EXPECT_TRUE(wr::find_wired_candidate(approved, found, scratch.path));

    // Two approved phones that share the accessory identity: only the
    // enrolment port or the last-used port identifies the phone.
    auto twin = identity;
    twin.usb_serial = "other-native-serial";
    const std::vector<aa::trust::ApprovedPhone> twins{
        {{17}, identity, aa::trust::IdentityStrength::serial_backed},
        {{18}, twin, aa::trust::IdentityStrength::serial_backed}};
    EXPECT_FALSE(wr::find_wired_candidate(twins, {}, scratch.path));
    const auto remembered = wr::find_wired_candidate(twins, found, scratch.path);
    ASSERT_TRUE(remembered);
    EXPECT_EQ(remembered->phone.value, 17U);
    EXPECT_EQ(wr::find_wired_candidates(twins, found, scratch.path).size(), 1U);
}

TEST(ManagedWiredWorker, StopReapsWorkerAndKeepsTheTwoSupervisorListeners) {
    Scratch scratch;
    ASSERT_FALSE(scratch.path.empty());
    const auto video_path = scratch.path / "video.sock";
    const auto input_path = scratch.path / "input.sock";
    auto video = aa::ipc::VideoServer::listen({video_path.string()}, {{}, aa::ipc::current_uid()});
    auto input = aa::ipc::InputServer::listen({input_path.string()}, {aa::ipc::current_uid()});
    ASSERT_TRUE(video);
    ASSERT_TRUE(input);

    const auto worker_script = scratch.path / "fake-receiver";
    const auto ready_fifo = scratch.path / "ready";
    ASSERT_EQ(::mkfifo(ready_fifo.c_str(), 0600), 0);
    // Open reader side nonblocking before spawn so writer never deadlocks on startup.
    const int ready_rd = ::open(ready_fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ASSERT_GE(ready_rd, 0);
    {
        // Use a backgrounded sleep + `wait` (POSIX-interruptible by trapped
        // SIGTERM, unlike `read -t` which defers traps in bash 5.2) and a
        // builtin redirect in the trap so SIGTERM writes the marker without
        // spawning an external binary under CI load.
        std::ofstream fixture{worker_script};
        fixture << "#!/bin/bash\n"
                   "set -eu\n"
                   "[[ -S /proc/self/fd/3 && -S /proc/self/fd/4 ]]\n"
                   "dir=${0%/*}\n"
                   "sleeper=''\n"
                   "trap 'printf stopped > \"$dir/stopped\"; "
                   "[[ -n \"$sleeper\" ]] && kill \"$sleeper\" 2>/dev/null || true; exit 0' TERM\n"
                   "sleep 5 &\n"
                   "sleeper=$!\n"
                   "printf 'ready\\n' > \"$dir/ready\"\n"
                   "wait \"$sleeper\" || true\n";
        ASSERT_TRUE(fixture);
    }
    ASSERT_EQ(::chmod(worker_script.c_str(), 0700), 0);

    wr::WiredWorker worker;
    ASSERT_TRUE(worker.start(worker_script, {{17}, "1-2.3"}, scratch.path / "store",
                             input.value().native_handle()));
    // Synchronize on the ready FIFO via poll and drain the readiness line
    // before signalling SIGTERM.
    {
        std::array<char, 32> buf{};
        std::string ready_msg;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (ready_msg.find("ready\n") == std::string::npos) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            ASSERT_GT(left.count(), 0) << "worker fixture never signalled ready";
            pollfd pfd{ready_rd, POLLIN, 0};
            int poll_rc;
            do {
                poll_rc = ::poll(&pfd, 1, static_cast<int>(left.count()));
            } while (poll_rc < 0 && errno == EINTR);
            ASSERT_GT(poll_rc, 0);
            const ssize_t n = ::read(ready_rd, buf.data(), buf.size());
            if (n > 0) {
                ready_msg.append(buf.data(), static_cast<std::size_t>(n));
            } else if (n == 0) {
                break;
            }
        }
        ASSERT_NE(ready_msg.find("ready\n"), std::string::npos);
    }
    ::close(ready_rd);

    const auto stop_started = std::chrono::steady_clock::now();
    worker.stop();
    const auto stop_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_started);
    EXPECT_FALSE(worker.running());
    EXPECT_TRUE(std::filesystem::exists(scratch.path / "stopped"))
        << "worker did not run SIGTERM trap before reap (stop took " << stop_elapsed.count()
        << " ms)";
    EXPECT_LT(stop_elapsed.count(), 700)
        << "worker.stop() appears to have fallen through to SIGKILL";
    auto inherited_video = aa::ipc::VideoServer::adopt_listener(
        ::dup(video.value().native_handle()), {{}, aa::ipc::current_uid()});
    auto inherited_input = aa::ipc::InputServer::adopt_listener(
        ::dup(input.value().native_handle()), {aa::ipc::current_uid()});
    ASSERT_TRUE(inherited_video);
    ASSERT_TRUE(inherited_input);
    inherited_video.value().close();
    inherited_input.value().close();
    EXPECT_TRUE(std::filesystem::is_socket(video_path));
    EXPECT_TRUE(std::filesystem::is_socket(input_path));
}

TEST(ManagedWiredWorker, LateClientConnectSurvivesFailedFirstWorkerAndReceivesFrameFromSecond) {
    Scratch scratch;
    ASSERT_FALSE(scratch.path.empty());
    const auto video_path = scratch.path / "video.sock";
    const auto input_path = scratch.path / "input.sock";
    auto video = aa::ipc::VideoServer::listen({video_path.string()}, {{}, aa::ipc::current_uid()});
    auto input = aa::ipc::InputServer::listen({input_path.string()}, {aa::ipc::current_uid()});
    ASSERT_TRUE(video);
    ASSERT_TRUE(input);
    wr::ManagedVideoBroker broker;

    // Worker 1 starts BEFORE the display connects (P1-1), synchronized via FIFO.
    const auto worker1_script = scratch.path / "worker1-fail";
    const auto started_fifo = scratch.path / "w1-started";
    const auto gate_fifo = scratch.path / "w1-gate";
    ASSERT_EQ(::mkfifo(started_fifo.c_str(), 0600), 0);
    ASSERT_EQ(::mkfifo(gate_fifo.c_str(), 0600), 0);
    const int started_rd = ::open(started_fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ASSERT_GE(started_rd, 0);
    // Hold gate write end open until after client connects and handoff is sent.
    const int gate_rd_keep = ::open(gate_fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    const int gate_wr = ::open(gate_fifo.c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_GE(gate_rd_keep, 0);
    ASSERT_GE(gate_wr, 0);
    {
        std::ofstream fixture{worker1_script};
        fixture << "#!/bin/bash\n"
                   "set -eu\n"
                   "[[ -S /proc/self/fd/3 && -S /proc/self/fd/4 ]]\n"
                   "dir=${0%/*}\n"
                   "printf 'up\\n' > \"$dir/w1-started\"\n"
                   "read -r -t 5 < \"$dir/w1-gate\"\n"
                   "exit 3\n";
        ASSERT_TRUE(fixture);
    }
    ASSERT_EQ(::chmod(worker1_script.c_str(), 0700), 0);

    wr::WiredWorker worker1;
    ASSERT_TRUE(worker1.start(worker1_script, {{17}, "1-2.3"}, scratch.path / "store",
                              input.value().native_handle()));
    {
        pollfd pfd{started_rd, POLLIN, 0};
        int rc;
        do { rc = ::poll(&pfd, 1, 5000); } while (rc < 0 && errno == EINTR);
        ASSERT_GT(rc, 0);
    }
    ::close(started_rd);

    // Display connects AFTER worker 1 is already running. The supervisor (not
    // the worker) accepts the consumer and hands it off over fd 3.
    auto consumer = connect_broker_consumer({video_path.string()}, video.value(), broker);
    ASSERT_TRUE(consumer);
    ASSERT_TRUE(worker1.send_video_client(broker.client_fd(), broker.state_fd(), broker.limits()));

    // Release worker 1 so it fails (exit 3) and reap synchronously via waitpid.
    ASSERT_EQ(::write(gate_wr, "go\n", 3), 3);
    ::close(gate_wr);
    ::close(gate_rd_keep);
    ASSERT_TRUE(worker1.wait_exit());
    EXPECT_FALSE(worker1.running());

    // The supervisor's connection to the display survived worker 1's failure.
    EXPECT_FALSE(broker.poll(video.value()));
    EXPECT_TRUE(broker.has_connection());
    const auto still_open = consumer.value().receive(aa::core::Milliseconds{0});
    ASSERT_FALSE(still_open);
    EXPECT_EQ(still_open.error().code(), aa::ErrorCode::timeout);

    // Worker 2 (separate child process) receives the SAME connection via
    // SCM_RIGHTS handoff and delivers a real H.264 IDR access unit.
    int handoff[2]{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, handoff), 0);
    ASSERT_TRUE(aa::tools::send_video_handoff_install(
        handoff[0], broker.client_fd(), broker.state_fd(), broker.limits()));

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(handoff[0]);
        auto event = aa::tools::receive_video_handoff(handoff[1], aa::core::Milliseconds{2000});
        if (!event.has_value() || event.value().kind != aa::tools::VideoHandoffKind::install ||
            !event.value().producer.has_value()) {
            ::_exit(10);
        }
        wired_rx::VideoOutlet outlet{[](const std::string&) {}};
        outlet.install(std::move(*event.value().producer));
        const auto idr = to_bytes({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x99});
        aa::protocol::MessageHeader hdr{};
        hdr.timestamp = aa::core::Nanoseconds{1'000'000};
        if (!outlet.on_message({hdr, idr}).has_value()) ::_exit(11);
        outlet.close();
        ::close(handoff[1]);
        ::_exit(0);
    }
    ::close(handoff[1]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    ::close(handoff[0]);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // The original VideoConsumer receives worker 2's IDR cleanly.
    const auto rx = receive_complete(consumer.value());
    ASSERT_TRUE(rx);
    EXPECT_EQ(consumer.value().access_unit().frame, 1U);
    EXPECT_EQ(consumer.value().access_unit().payload,
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x99}));
    EXPECT_EQ(consumer.value().rejected(), 0U);

    // Explicit release_connection closes with SHUT_WR -> consumer sees EOF.
    broker.release_connection(&video.value());
    const auto closed = consumer.value().receive(aa::core::Milliseconds{500});
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code(), aa::ErrorCode::transport_closed);
}

TEST(ManagedWiredWorker, ShortStreamingWorkerHandsOffMonotonicIdsToReplacementChildProcess) {
    Scratch scratch;
    ASSERT_FALSE(scratch.path.empty());
    const auto video_path = scratch.path / "video.sock";
    auto video = aa::ipc::VideoServer::listen({video_path.string()}, {{}, aa::ipc::current_uid()});
    ASSERT_TRUE(video);
    wr::ManagedVideoBroker broker;

    auto consumer = connect_broker_consumer({video_path.string()}, video.value(), broker);
    ASSERT_TRUE(consumer);

    // Child process 1 sends a real IDR + delta through VideoOutlet and exits
    // abruptly (simulating a short wired run that exits within 30 s).
    const auto spawn_sender = [&](std::uint8_t tag, std::int64_t ts_ns, bool send_delta) {
        int handoff[2]{-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, handoff), 0);
        ASSERT_TRUE(aa::tools::send_video_handoff_install(
            handoff[0], broker.client_fd(), broker.state_fd(), broker.limits()));
        const pid_t child = ::fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            ::close(handoff[0]);
            auto event =
                aa::tools::receive_video_handoff(handoff[1], aa::core::Milliseconds{2000});
            if (!event.has_value() || !event.value().producer.has_value()) ::_exit(10);
            wired_rx::VideoOutlet outlet{[](const std::string&) {}};
            outlet.install(std::move(*event.value().producer));
            const auto idr = to_bytes({0x00, 0x00, 0x00, 0x01, 0x65, tag});
            aa::protocol::MessageHeader hdr{};
            hdr.timestamp = aa::core::Nanoseconds{ts_ns};
            if (!outlet.on_message({hdr, idr}).has_value()) ::_exit(11);
            if (send_delta) {
                const auto delta = to_bytes({0x00, 0x00, 0x00, 0x01, 0x41, tag});
                hdr.timestamp = aa::core::Nanoseconds{ts_ns + 33'000'000};
                if (!outlet.on_message({hdr, delta}).has_value()) ::_exit(12);
            }
            outlet.close();
            ::close(handoff[1]);
            ::_exit(0);
        }
        ::close(handoff[1]);
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        ::close(handoff[0]);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    };

    spawn_sender(0x11, 1'000'000, /*send_delta=*/true);
    ASSERT_TRUE(receive_complete(consumer.value()));
    const auto frame1 = consumer.value().access_unit().frame;
    const auto seq1 = consumer.value().access_unit().sequence;
    EXPECT_EQ(consumer.value().access_unit().payload.back(), 0x11U);

    ASSERT_TRUE(receive_complete(consumer.value()));
    const auto frame2 = consumer.value().access_unit().frame;
    const auto seq2 = consumer.value().access_unit().sequence;
    EXPECT_GT(frame2, frame1);
    EXPECT_GT(seq2, seq1);

    // Replacement child process 2 adopts the SAME connection with a fresh
    // VideoOutlet and sends an IDR. Frame ID and sequence continue monotonically.
    spawn_sender(0x22, 200'000'000, /*send_delta=*/false);
    ASSERT_TRUE(receive_complete(consumer.value()));
    EXPECT_GT(consumer.value().access_unit().frame, frame2);
    EXPECT_GT(consumer.value().access_unit().sequence, seq2);
    EXPECT_EQ(consumer.value().access_unit().payload.back(), 0x22U);
    EXPECT_EQ(consumer.value().rejected(), 0U);
}

TEST(ManagedWiredWorker, ChildHandoffPreservesUnequalLimitsAndDeliversFragmentedIdr) {
    Scratch scratch;
    ASSERT_FALSE(scratch.path.empty());
    const auto video_path = scratch.path / "video.sock";
    auto video = aa::ipc::VideoServer::listen({video_path.string()}, {{}, aa::ipc::current_uid()});
    ASSERT_TRUE(video);
    wr::ManagedVideoBroker broker;

    // Negotiate explicitly unequal limits (4 KiB datagram, 128 KiB AU, 192 KiB
    // socket budget) and send a 32 KiB IDR (8+ fragments) through a real child
    // SCM_RIGHTS handoff to catch any datagram/AU field swap.
    aa::ipc::NegotiatedLimits custom{};
    custom.max_datagram_payload = 4096U;
    custom.max_access_unit_bytes = 128U * 1024U;
    custom.socket_budget_bytes = 192U * 1024U;
    auto consumer = connect_broker_consumer({video_path.string()}, video.value(), broker, custom);
    ASSERT_TRUE(consumer);
    ASSERT_EQ(broker.limits(), custom);

    constexpr std::size_t kIdrBytes = 32U * 1024U + 137U;
    const auto expected_idr = make_fragmented_idr(kIdrBytes, 0x3AU);

    int handoff[2]{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, handoff), 0);
    ASSERT_TRUE(aa::tools::send_video_handoff_install(
        handoff[0], broker.client_fd(), broker.state_fd(), broker.limits()));

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(handoff[0]);
        auto event = aa::tools::receive_video_handoff(handoff[1], aa::core::Milliseconds{2000});
        if (!event.has_value() || event.value().kind != aa::tools::VideoHandoffKind::install ||
            !event.value().producer.has_value()) {
            ::_exit(10);
        }
        if (event.value().producer->limits() != custom) {
            ::_exit(11);
        }
        wired_rx::VideoOutlet outlet{[](const std::string&) {}};
        outlet.install(std::move(*event.value().producer));
        aa::protocol::MessageHeader hdr{};
        hdr.timestamp = aa::core::Nanoseconds{1'000'000};
        if (!outlet.on_message({hdr, expected_idr}).has_value()) {
            ::_exit(12);
        }
        outlet.close();
        ::close(handoff[1]);
        ::_exit(0);
    }
    ::close(handoff[1]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    ::close(handoff[0]);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child status=" << status;

    const auto rx = receive_complete(consumer.value());
    ASSERT_TRUE(rx);
    ASSERT_EQ(consumer.value().access_unit().payload.size(), expected_idr.size());
    for (std::size_t i = 0; i < expected_idr.size(); ++i) {
        ASSERT_EQ(consumer.value().access_unit().payload[i],
                  std::to_integer<std::uint8_t>(expected_idr[i]))
            << "mismatch at byte " << i;
    }
    EXPECT_EQ(consumer.value().rejected(), 0U);

    // Also verify default limits (16 KiB datagram, 4 MiB AU) deliver a 32 KiB+
    // fragmented IDR through handoff without swapping the two caps.
    auto default_consumer =
        connect_broker_consumer({video_path.string()}, video.value(), broker);
    ASSERT_TRUE(default_consumer);
    ASSERT_EQ(broker.limits(), aa::ipc::NegotiatedLimits{});
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, handoff), 0);
    ASSERT_TRUE(aa::tools::send_video_handoff_install(
        handoff[0], broker.client_fd(), broker.state_fd(), broker.limits()));
    const pid_t child2 = ::fork();
    ASSERT_GE(child2, 0);
    if (child2 == 0) {
        ::close(handoff[0]);
        auto event = aa::tools::receive_video_handoff(handoff[1], aa::core::Milliseconds{2000});
        if (!event.has_value() || !event.value().producer.has_value() ||
            event.value().producer->limits() != aa::ipc::NegotiatedLimits{}) {
            ::_exit(20);
        }
        wired_rx::VideoOutlet outlet{[](const std::string&) {}};
        outlet.install(std::move(*event.value().producer));
        aa::protocol::MessageHeader hdr{};
        hdr.timestamp = aa::core::Nanoseconds{2'000'000};
        if (!outlet.on_message({hdr, expected_idr}).has_value()) {
            ::_exit(21);
        }
        outlet.close();
        ::close(handoff[1]);
        ::_exit(0);
    }
    ::close(handoff[1]);
    status = 0;
    while (::waitpid(child2, &status, 0) < 0 && errno == EINTR) {}
    ::close(handoff[0]);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child2 status=" << status;

    ASSERT_TRUE(receive_complete(default_consumer.value()));
    EXPECT_EQ(default_consumer.value().access_unit().payload.size(), expected_idr.size());
    EXPECT_EQ(default_consumer.value().rejected(), 0U);

    // Sender and receiver reject invalid/swapped limits.
    aa::ipc::NegotiatedLimits swapped{};
    swapped.max_datagram_payload = static_cast<std::uint32_t>(aa::ipc::kMaxAccessUnitBytes);
    swapped.max_access_unit_bytes = static_cast<std::uint32_t>(aa::ipc::kMaxDatagramPayload);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, handoff), 0);
    EXPECT_FALSE(aa::tools::send_video_handoff_install(
        handoff[0], broker.client_fd(), broker.state_fd(), swapped));
    ::close(handoff[0]);
    ::close(handoff[1]);
}

} // namespace
