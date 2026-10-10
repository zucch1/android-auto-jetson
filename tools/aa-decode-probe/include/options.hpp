// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aa::decode_probe {

struct ProbeOptions {
    std::string profile = "1280x720@30";
    std::string bitrate = "10M";
    int duration_s = 600;
    std::string fixture_path;
    std::string forced_decoder;
    std::string output_json_path;
    bool help = false;

    // Parsed profile details
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint64_t bitrate_bps = 10'000'000;
};

bool parse_options(int argc, char* argv[], ProbeOptions& options, std::string& error_msg);
void print_help(const char* prog_name);

} // namespace aa::decode_probe
