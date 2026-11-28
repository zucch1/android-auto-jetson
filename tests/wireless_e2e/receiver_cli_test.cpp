// SPDX-License-Identifier: GPL-3.0-or-later
// CLI option-parsing regressions for aa-wireless-receiver: compiled under both
// production (AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS=0) and test-only
// (AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS=1) configurations (#168).
#include "../../tools/wireless-receiver/Options.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <utility>

namespace wr = aa::wireless_receiver;

TEST(WirelessReceiverCli, AcceptsBaselinePhoneAndPairingOptions) {
    std::string error;
    const std::array phone_argv{"aa-wireless-receiver", "--store-dir", "/tmp", "--phone-id", "1"};
    const auto phone_opts =
        wr::parse_options(static_cast<int>(phone_argv.size()), phone_argv.data(), &error);
    ASSERT_TRUE(phone_opts.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(phone_opts->phone.value, 1U);
    EXPECT_FALSE(phone_opts->pairing_only);

    const std::array pairing_argv{"aa-wireless-receiver", "--store-dir", "/tmp", "--pairing-only"};
    const auto pairing_opts =
        wr::parse_options(static_cast<int>(pairing_argv.size()), pairing_argv.data(), &error);
    ASSERT_TRUE(pairing_opts.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(pairing_opts->pairing_only);
}

// #173 / #228: the catch-up watermark only acts between the drift high
// watermark (20 ms) and the router's 200 ms late cap; 0 disables catch-up,
// and a nonzero value requires the legacy arrival model (--audio-playout-ms 0).
TEST(WirelessReceiverCli, CatchUpWatermarkAcceptsOnlyEffectiveValues) {
    const auto parse = [](const char *watermark, const char *playout_ms) {
        const std::array argv{"aa-wireless-receiver",          "--store-dir", "/tmp",
                              "--phone-id",                    "1",           "--audio-playout-ms",
                              playout_ms,                      "--audio-catch-up-watermark-ms",
                              watermark};
        return wr::parse_options(static_cast<int>(argv.size()), argv.data());
    };
    for (const auto &[text, value] : {std::pair{"0", 0}, std::pair{"20", 20}, std::pair{"90", 90},
                                      std::pair{"200", 200}}) {
        const auto options = parse(text, "0");
        ASSERT_TRUE(options.has_value()) << text;
        EXPECT_EQ(options->audio_catch_up_watermark_ms, value);
    }
    for (const auto *text : {"1", "19", "201", "1000", "-5", "abc", "90ms", "18446744073709551616"}) {
        EXPECT_FALSE(parse(text, "0").has_value()) << text;
    }
    EXPECT_TRUE(parse("0", "120").has_value());
    EXPECT_FALSE(parse("90", "120").has_value());

    const std::array defaults{"aa-wireless-receiver", "--store-dir", "/tmp", "--phone-id", "1"};
    const auto options = wr::parse_options(static_cast<int>(defaults.size()), defaults.data());
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->audio_catch_up_watermark_ms, 0);
}

#if defined(AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS) && AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS

TEST(WirelessReceiverCli, TestBuildAllowsHelperBusAddressOverride) {
    EXPECT_TRUE(wr::kAllowHelperBusAddress);
    EXPECT_NE(std::string_view{wr::usage}.find("--helper-bus-address"), std::string_view::npos);

    std::string error;
    const std::array phone_argv{"aa-wireless-receiver",
                                "--store-dir",
                                "/tmp",
                                "--phone-id",
                                "1",
                                "--helper-bus-address",
                                "unix:path=/tmp/fake-helper"};
    const auto phone_opts =
        wr::parse_options(static_cast<int>(phone_argv.size()), phone_argv.data(), &error);
    ASSERT_TRUE(phone_opts.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(phone_opts->helper_address, "unix:path=/tmp/fake-helper");

    const std::array pairing_argv{
        "aa-wireless-receiver", "--store-dir",          "/tmp",
        "--pairing-only",       "--helper-bus-address", "unix:path=/tmp/fake-helper"};
    const auto pairing_opts =
        wr::parse_options(static_cast<int>(pairing_argv.size()), pairing_argv.data(), &error);
    ASSERT_TRUE(pairing_opts.has_value());
    EXPECT_EQ(pairing_opts->helper_address, "unix:path=/tmp/fake-helper");

    const std::array baseline_argv{"aa-wireless-receiver", "--store-dir", "/tmp", "--phone-id",
                                   "1"};
    const auto baseline_opts =
        wr::parse_options(static_cast<int>(baseline_argv.size()), baseline_argv.data(), &error);
    ASSERT_TRUE(baseline_opts.has_value());
    EXPECT_TRUE(baseline_opts->helper_address.empty());
}

TEST(WirelessReceiverCli, TestBuildRejectsMalformedHelperBusAddressValue) {
    const std::array missing_val{"aa-wireless-receiver", "--store-dir", "/tmp",
                                 "--phone-id",           "1",           "--helper-bus-address"};
    EXPECT_FALSE(wr::parse_options(static_cast<int>(missing_val.size()), missing_val.data()));

    const std::array flag_as_val{"aa-wireless-receiver", "--store-dir",  "/tmp", "--phone-id", "1",
                                 "--helper-bus-address", "--diagnostics"};
    EXPECT_FALSE(wr::parse_options(static_cast<int>(flag_as_val.size()), flag_as_val.data()));
}

#else

TEST(WirelessReceiverCli, ProductionBuildRejectsHelperBusAddressFlag) {
    EXPECT_FALSE(wr::kAllowHelperBusAddress);
    EXPECT_EQ(std::string_view{wr::usage}.find("--helper-bus-address"), std::string_view::npos);

    std::string error;
    const std::array phone_argv{"aa-wireless-receiver",
                                "--store-dir",
                                "/tmp",
                                "--phone-id",
                                "1",
                                "--helper-bus-address",
                                "unix:path=/tmp/fake-helper"};
    EXPECT_FALSE(wr::parse_options(static_cast<int>(phone_argv.size()), phone_argv.data(), &error));
    EXPECT_NE(error.find("--helper-bus-address is only supported in test builds"),
              std::string::npos);

    error.clear();
    const std::array pairing_argv{
        "aa-wireless-receiver", "--store-dir",          "/tmp",
        "--pairing-only",       "--helper-bus-address", "unix:path=/tmp/fake-helper"};
    EXPECT_FALSE(
        wr::parse_options(static_cast<int>(pairing_argv.size()), pairing_argv.data(), &error));
    EXPECT_NE(error.find("--helper-bus-address is only supported in test builds"),
              std::string::npos);

    error.clear();
    const std::array missing_val{"aa-wireless-receiver", "--store-dir", "/tmp",
                                 "--phone-id",           "1",           "--helper-bus-address"};
    EXPECT_FALSE(
        wr::parse_options(static_cast<int>(missing_val.size()), missing_val.data(), &error));
    EXPECT_NE(error.find("--helper-bus-address is only supported in test builds"),
              std::string::npos);

    error.clear();
    const std::array equals_form{
        "aa-wireless-receiver", "--store-dir", "/tmp",
        "--phone-id",           "1",           "--helper-bus-address=unix:path=/tmp/fake-helper"};
    EXPECT_FALSE(
        wr::parse_options(static_cast<int>(equals_form.size()), equals_form.data(), &error));
    EXPECT_NE(error.find("--helper-bus-address is only supported in test builds"),
              std::string::npos);
}

#endif
