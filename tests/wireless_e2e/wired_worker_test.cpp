// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wired-receiver/VideoOutlet.hpp"
#include "../../tools/wireless-receiver/ManagedVideo.hpp"
#include "../../tools/wireless-receiver/WiredWorker.hpp"

#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/VideoChannel.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
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
                                                          int max_steps = 16) {
    for (int i = 0; i < max_steps; ++i) {
        auto step = consumer.receive(aa::core::Milliseconds{2000});
        if (!step) return step;
        if (step.value() == aa::ipc::ReceiveStatus::complete) return step;
    }
    return aa::Error{aa::ErrorCode::timeout};
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
    const auto wait_fifo = scratch.path / "wait";
    ASSERT_EQ(::mkfifo(ready_fifo.c_str(), 0600), 0);
    ASSERT_EQ(::mkfifo(wait_fifo.c_str(), 0600), 0);
    // Open reader side nonblocking before spawn so writer never deadlocks on startup.
    const int ready_rd = ::open(ready_fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ASSERT_GE(ready_rd, 0);
    {
        std::ofstream fixture{worker_script};
        fixture << "#!/bin/bash\n"
                   "set -eu\n"
                   "[[ -S /proc/self/fd/3 && -S /proc/self/fd/4 ]]\n"
                   "dir=${0%/*}\n"
                   "trap 'touch \"$dir/stopped\"; exit 0' TERM\n"
                   "printf 'ready\\n' > \"$dir/ready\"\n"
                   "exec 5<>\"$dir/wait\"\n"
                   "read -r -t 5 -u 5\n";
        ASSERT_TRUE(fixture);
    }
    ASSERT_EQ(::chmod(worker_script.c_str(), 0700), 0);

    wr::WiredWorker worker;
    ASSERT_TRUE(worker.start(worker_script, {{17}, "1-2.3"}, scratch.path / "store",
                             input.value().native_handle()));
    // Synchronize on the ready FIFO via poll (no timed sleep loop).
    pollfd pfd{ready_rd, POLLIN, 0};
    int poll_rc;
    do {
        poll_rc = ::poll(&pfd, 1, 5000);
    } while (poll_rc < 0 && errno == EINTR);
    ASSERT_GT(poll_rc, 0);
    ::close(ready_rd);

    worker.stop();
    EXPECT_FALSE(worker.running());
    EXPECT_TRUE(std::filesystem::exists(scratch.path / "stopped"));
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
    aa::core::Result<aa::ipc::VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::thread connector([&] {
        consumer = aa::ipc::connect_video_consumer(
            {video_path.string()}, {{}, aa::ipc::current_uid(), aa::core::Milliseconds{2000}});
    });
    while (!broker.has_connection()) {
        pollfd pfd{video.value().native_handle(), POLLIN, 0};
        if (!broker.has_pending_handshake(video.value())) {
            (void)::poll(&pfd, 1, 500);
        }
        (void)broker.poll(video.value());
    }
    connector.join();
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

    aa::core::Result<aa::ipc::VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::thread connector([&] {
        consumer = aa::ipc::connect_video_consumer(
            {video_path.string()}, {{}, aa::ipc::current_uid(), aa::core::Milliseconds{2000}});
    });
    while (!broker.has_connection()) {
        pollfd pfd{video.value().native_handle(), POLLIN, 0};
        if (!broker.has_pending_handshake(video.value())) {
            (void)::poll(&pfd, 1, 500);
        }
        (void)broker.poll(video.value());
    }
    connector.join();
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

} // namespace
