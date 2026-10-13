// SPDX-License-Identifier: GPL-3.0-or-later
// AOA switch contract tests (task 28): the reference handshake() sequence of
// tools/target/usb_aoa_libusb.py driven through a scripted control channel.
// Every protocol-contract failure yields a typed error and a filled report.

#include "../src/transport/usb_adapter/UsbAoaSwitch.hpp"

#include "UsbAoaFakes.hpp"

#include <aa/core/Error.hpp>

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace ua = aa::transport::usb_adapter;
namespace fx = aa::test::usb_aoa;
using aa::ErrorCode;

TEST(UsbAoaSwitch, HappyPathSwitchesWithFullLengths) {
    fx::FakeControlChannel channel;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(report.started);
    EXPECT_TRUE(report.failed_at.empty());
    EXPECT_EQ(report.protocol_version, 2);
    ASSERT_EQ(report.steps.size(), 6u);
    for (const auto &step : report.steps) {
        EXPECT_EQ(step.observed_bytes, step.expected_bytes);
    }
    EXPECT_EQ(report.requests, 8);
    ASSERT_EQ(channel.calls.size(), 8u);
    EXPECT_EQ(channel.calls[0].request, ua::kAoaRequestGetProtocol);
    EXPECT_EQ(channel.calls[0].length, 2u);
    for (std::uint16_t index = 0; index < 6; ++index) {
        EXPECT_EQ(channel.calls[1 + index].request, ua::kAoaRequestSendString);
        EXPECT_EQ(channel.calls[1 + index].index, index);
    }
    EXPECT_EQ(channel.calls[7].request, ua::kAoaRequestStart);
    EXPECT_EQ(channel.calls[7].length, 0u);
}

TEST(UsbAoaSwitch, SendStringPayloadIsNulTerminatedAscii) {
    auto payload = ua::aoa_string_payload("Android Auto");
    ASSERT_TRUE(payload.has_value());
    ASSERT_EQ(payload.value().size(), 13u);
    EXPECT_EQ(payload.value().back(), std::byte{0});
    EXPECT_EQ(payload.value()[0], std::byte{'A'});
}

TEST(UsbAoaSwitch, RejectsProtocolLengthMismatch) {
    for (const std::size_t length : {std::size_t{1}, std::size_t{3}, std::size_t{0}}) {
        fx::FakeControlChannel channel;
        channel.protocol_length = length;
        aa::transport::AoaSwitchReport report;

        auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::transport_malformed_frame);
        EXPECT_EQ(report.failed_at, "get-protocol-length");
        EXPECT_FALSE(report.started);
    }
}

TEST(UsbAoaSwitch, RejectsProtocolVersionBelowOne) {
    for (const std::uint16_t version : {std::uint16_t{0}}) {
        fx::FakeControlChannel channel;
        channel.protocol_version = version;
        aa::transport::AoaSwitchReport report;

        auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::transport_unavailable);
        EXPECT_EQ(report.failed_at, "get-protocol-version");
    }
}

TEST(UsbAoaSwitch, AcceptsGarbageVersionBytesWithoutCrash) {
    // The contract only requires version >= 1; 0xFFFF garbage is accepted as a
    // valid (hostile) version and must not corrupt the handshake state.
    fx::FakeControlChannel channel;
    channel.protocol_version = 0xFFFF;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(report.protocol_version, 0xFFFF);
    EXPECT_TRUE(report.started);
}

TEST(UsbAoaSwitch, RejectsShortSendString) {
    fx::FakeControlChannel channel;
    channel.send_string_short_by = 1;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(report.failed_at, "send-string-manufacturer");
    ASSERT_EQ(report.steps.size(), 1u);
    EXPECT_EQ(report.steps[0].expected_bytes, 8u);
    EXPECT_EQ(report.steps[0].observed_bytes, 7u);
}

TEST(UsbAoaSwitch, RejectsNonZeroStart) {
    fx::FakeControlChannel channel;
    channel.start_transferred = 1;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(report.failed_at, "start-short");
}

TEST(UsbAoaSwitch, ControlInFailureIsTypedIoError) {
    fx::FakeControlChannel channel;
    channel.fail_control_in = true;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(report.failed_at, "get-protocol-io");
}

TEST(UsbAoaSwitch, ControlOutFailureIsTypedIoError) {
    fx::FakeControlChannel channel;
    channel.fail_control_out = true;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, aa::transport::AoaAccessoryInfo{}, report);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(report.failed_at, "send-string-manufacturer-io");
}

TEST(UsbAoaSwitch, RejectsNonAsciiAccessoryStrings) {
    aa::transport::AoaAccessoryInfo info;
    info.model = std::string("Android\xFF"
                             "Auto");
    fx::FakeControlChannel channel;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, info, report);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::invalid_argument);
}

TEST(UsbAoaSwitch, RejectsOverBudgetAccessoryStrings) {
    aa::transport::AoaAccessoryInfo info;
    info.manufacturer = std::string(aa::trust::kMaxIdentityFieldBytes + 1, 'm');
    fx::FakeControlChannel channel;
    aa::transport::AoaSwitchReport report;

    auto result = ua::run_aoa_switch(channel, info, report);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::invalid_argument);
}

} // namespace
