// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace aa::config {

// Configuration boundary ("parse, don't validate"): untrusted text/env values
// are turned into this typed value exactly once, by validate(); everything
// inside the receiver consumes only the validated Config. Immutable after
// validation and owned by the process entry point.
//
// Fail-closed rules (wireless startup, plan scope):
//  - the wireless jurisdiction must be explicitly confirmed or overridden at
//    install time; an unconfirmed jurisdiction never starts the AP;
//  - the fixed AP channel is 36 (planned 5 GHz design) and is validated, not
//    taken from the phone;
//  - microphone and sensor capabilities stay disabled until their qualification
//    exists (tasks 25/26); they cannot be enabled by a phone.
struct Wireless final {
    std::string jurisdiction{};   // planned deployment: "US"
    bool jurisdiction_confirmed{false};
    std::uint8_t channel{36};
};

struct Config final {
    std::filesystem::path credential_path{};  // HU_KEY_PATH (task 8 loader)
    Wireless wireless{};
    bool microphone_enabled{false};
    bool sensors_enabled{false};
};

// The one boundary constructor. Returns config_jurisdiction_unconfirmed when
// the jurisdiction was never confirmed and config_invalid for the rest.
[[nodiscard]] core::Result<Config> validate(Config candidate);

} // namespace aa::config
