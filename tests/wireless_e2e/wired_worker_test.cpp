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
    EXPECT_FALSE(wr::find_wired_candidate(approved, {}, scratch.path));
    EXPECT_TRUE(wr::find_wired_candidate(approved, found, scratch.path));
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

} // namespace
