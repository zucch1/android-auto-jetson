// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wireless-receiver/WiredWorker.hpp"

#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/VideoChannel.hpp>

#include <cstdlib>
#include <gtest/gtest.h>
#include <sys/stat.h>

namespace {

namespace wr = aa::wireless_receiver;
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
    const auto fifo = scratch.path / "wait";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    {
        std::ofstream fixture{worker_script};
        fixture << "#!/bin/bash\n"
                   "set -eu\n"
                   "[[ -S /proc/self/fd/3 && -S /proc/self/fd/4 ]]\n"
                   "dir=${0%/*}\n"
                   "trap 'touch \"$dir/stopped\"; exit 0' TERM\n"
                   "touch \"$dir/started\"\n"
                   "exec 5<>\"$dir/wait\"\n"
                   "read -r -t 5 -u 5\n";
        ASSERT_TRUE(fixture);
    }
    ASSERT_EQ(::chmod(worker_script.c_str(), 0700), 0);

    wr::WiredWorker worker;
    ASSERT_TRUE(worker.start(worker_script, {{17}, "1-2.3"}, scratch.path / "store",
                             video.value().native_handle(), input.value().native_handle()));
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!std::filesystem::exists(scratch.path / "started") &&
           worker.running() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(std::filesystem::exists(scratch.path / "started"));
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

TEST(ManagedWiredWorker, PassesPreAcceptedVideoClientWithoutClosingSupervisorConnection) {
    Scratch scratch;
    ASSERT_FALSE(scratch.path.empty());
    const auto video_path = scratch.path / "video.sock";
    const auto input_path = scratch.path / "input.sock";
    auto video = aa::ipc::VideoServer::listen({video_path.string()}, {{}, aa::ipc::current_uid()});
    auto input = aa::ipc::InputServer::listen({input_path.string()}, {aa::ipc::current_uid()});
    ASSERT_TRUE(video);
    ASSERT_TRUE(input);

    aa::core::Result<aa::ipc::VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::thread connector([&] {
        consumer = aa::ipc::connect_video_consumer(
            {video_path.string()}, {{}, aa::ipc::current_uid(), aa::core::Milliseconds{2000}});
    });
    auto producer = video.value().accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    ASSERT_TRUE(producer);
    ASSERT_TRUE(consumer);

    // Adopting a duplicated client fd and closing it must not shut down the
    // supervisor's open connection (the consumer does not see EOF).
    auto adopted = aa::ipc::VideoProducer::adopt_accepted(
        ::dup(producer.value().native_handle()), producer.value().limits());
    ASSERT_TRUE(adopted);
    adopted.value().close();
    const auto still_open = consumer.value().receive(aa::core::Milliseconds{20});
    ASSERT_FALSE(still_open);
    EXPECT_EQ(still_open.error().code(), aa::ErrorCode::timeout);

    // Spawning a wired worker with the pre-accepted client fd passes fd 5 and
    // --video-client-limits; when the worker exits quickly, the connection stays open.
    const auto worker_script = scratch.path / "fake-receiver";
    {
        std::ofstream fixture{worker_script};
        fixture << "#!/bin/bash\n"
                   "set -eu\n"
                   "[[ -S /proc/self/fd/3 && -S /proc/self/fd/4 && -S /proc/self/fd/5 ]]\n"
                   "dir=${0%/*}\n"
                   "printf '%s\\n' \"$@\" > \"$dir/args\"\n";
        ASSERT_TRUE(fixture);
    }
    ASSERT_EQ(::chmod(worker_script.c_str(), 0700), 0);

    wr::WiredWorker worker;
    ASSERT_TRUE(worker.start(worker_script, {{17}, "1-2.3"}, scratch.path / "store",
                             video.value().native_handle(), input.value().native_handle(),
                             producer.value().native_handle(), producer.value().limits()));
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (worker.running() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(worker.running());
    std::ifstream args_file{scratch.path / "args"};
    std::string args_text{std::istreambuf_iterator<char>{args_file}, {}};
    EXPECT_NE(args_text.find("--video-client-limits\n"), std::string::npos) << args_text;

    const auto after_worker_exit = consumer.value().receive(aa::core::Milliseconds{20});
    ASSERT_FALSE(after_worker_exit);
    EXPECT_EQ(after_worker_exit.error().code(), aa::ErrorCode::timeout);

    // Closing the supervisor's own producer cleanly signals EOF to the consumer.
    producer.value().close();
    const auto closed = consumer.value().receive(aa::core::Milliseconds{500});
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code(), aa::ErrorCode::transport_closed);
}

} // namespace
