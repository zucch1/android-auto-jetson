// SPDX-License-Identifier: GPL-3.0-or-later
// LibusbWorld contract tests (task-28 pre-merge fix round): the real adapter
// code driven against the fake libusb runtime (fake_libusb.cpp, loaded through
// the production dlopen path). F1: the pre-switch path must run the AOA switch
// over endpoint-0 control transfers WITHOUT detaching or claiming interface 0
// (a claim there can disrupt an existing binding or fail an otherwise valid
// switch), and the accessory-mode claim is what fails/succeeds independently.
// F5: reset_port_device() must honor the FULL selector, never resetting a
// replacement device that does not match it.

#include "../src/transport/usb_adapter/LibusbWorld.hpp"

#include "../src/transport/usb_adapter/UsbAoaEndpoint.hpp"

#include <aa/core/Error.hpp>
#include <aa/transport/UsbAoa.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

namespace tr = aa::transport;
namespace ua = aa::transport::usb_adapter;
using aa::ErrorCode;

const std::string &log_path() {
    static const std::string path = [] {
        const auto file = std::filesystem::temp_directory_path() /
                          ("aa_fake_libusb_" + std::to_string(::getpid()) + ".log");
        return file.string();
    }();
    return path;
}

void set_scenario(const char *scenario) {
    ::setenv("AA_FAKE_LIBUSB_SCENARIO", scenario, 1);
    ::setenv("AA_FAKE_LIBUSB_LOG", log_path().c_str(), 1);
    std::remove(log_path().c_str());
}

std::string read_log() {
    std::ifstream in{log_path()};
    std::ostringstream body;
    body << in.rdbuf();
    return body.str();
}

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

int count_of(const std::string &text, const std::string &needle) {
    int count = 0;
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        ++count;
        at += needle.size();
    }
    return count;
}

tr::UsbDeviceSelector phone_selector() {
    return tr::UsbDeviceSelector{"1-2.1", -1, -1, 0x04E8, 0x6860};
}

tr::WiredAdmission admit_all() {
    return [](const aa::trust::WiredIdentity &) { return true; };
}

TEST(UsbAoaWorld, PreSwitchClaimRefusalStillCompletesSwitch) {
    set_scenario("claim-fail-always");
    auto factory = ua::make_libusb_link_factory();
    ASSERT_TRUE(factory.has_value()) << "fake libusb harness must be on LD_LIBRARY_PATH";

    aa::transport::UsbDeviceSnapshot pre{};
    aa::transport::AoaSwitchReport report{};
    auto switched =
        factory.value()->switch_to_accessory(phone_selector(), tr::AoaAccessoryInfo{}, pre, report);

    ASSERT_TRUE(switched.has_value()) << "switch must not need an interface claim";
    EXPECT_TRUE(report.started);
    EXPECT_EQ(report.steps.size(), 6u);
    std::string log = read_log();
    EXPECT_TRUE(contains(log, "libusb_init"));
    EXPECT_TRUE(contains(log, "libusb_control_transfer:51:0"));
    EXPECT_TRUE(contains(log, "libusb_control_transfer:53:0"));
    EXPECT_EQ(count_of(log, "libusb_control_transfer:52:"), 6);
    EXPECT_FALSE(contains(log, "libusb_claim_interface"))
        << "pre-switch path must not claim interface 0";
    EXPECT_FALSE(contains(log, "libusb_detach_kernel_driver"))
        << "pre-switch path must not detach a kernel driver";

    aa::transport::UsbDeviceSnapshot post{};
    auto claimed = factory.value()->claim_accessory(phone_selector(), post);
    ASSERT_FALSE(claimed.has_value());
    EXPECT_EQ(claimed.error().code(), ErrorCode::transport_io);
    log = read_log();
    EXPECT_EQ(count_of(log, "libusb_claim_interface"), 1)
        << "the post-switch claim is what fails, independently of the switch";
}

TEST(UsbAoaWorld, PreSwitchClaimRefusalDoesNotBlockPipeline) {
    set_scenario("claim-fail-pre");
    auto factory = ua::make_libusb_link_factory();
    ASSERT_TRUE(factory.has_value()) << "fake libusb harness must be on LD_LIBRARY_PATH";

    auto link = ua::connect_aoa(std::move(factory.value()), phone_selector(),
                                tr::AoaAccessoryInfo{}, admit_all());

    ASSERT_TRUE(link.has_value())
        << "phone-mode claim refusal must not fail the switch or the accessory claim";
    EXPECT_TRUE(link.value().switch_report.started);
    std::string log = read_log();
    const std::size_t start = log.find("libusb_control_transfer:53:0");
    const std::size_t claim = log.find("libusb_claim_interface");
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(claim, std::string::npos);
    EXPECT_LT(start, claim) << "the claim must happen AFTER the AOA switch";
    EXPECT_EQ(count_of(log, "libusb_claim_interface"), 1);
}

TEST(UsbAoaWorld, ResetSkipsReplacementDeviceOutsideSelector) {
    set_scenario("replacement");

    auto reset = tr::reset_port_device(phone_selector());

    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(reset.error().code(), ErrorCode::transport_unavailable);
    std::string log = read_log();
    EXPECT_TRUE(contains(log, "libusb_init"));
    EXPECT_FALSE(contains(log, "libusb_reset_device"))
        << "a replacement device that does not match the selector must not be reset";

    tr::UsbDeviceSelector wrong_bus{"1-2.1", 2, -1, 0x04E8, 0x6860};
    auto reset_bus = tr::reset_port_device(wrong_bus);
    ASSERT_FALSE(reset_bus.has_value());
    EXPECT_FALSE(contains(read_log(), "libusb_reset_device"));
}

TEST(UsbAoaWorld, ResetMatchesFullSelectorAndResetsTarget) {
    set_scenario("happy");

    auto reset = tr::reset_port_device(phone_selector());

    ASSERT_TRUE(reset.has_value());
    EXPECT_TRUE(contains(read_log(), "libusb_reset_device"))
        << "a device that matches the full selector must be reset (test not vacuous)";
}

} // namespace
