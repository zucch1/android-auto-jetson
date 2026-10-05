#include "options.hpp"

#include <charconv>
#include <limits>
#include <string_view>
#include <cstring>
#include <iostream>
#include <regex>

namespace aa::decode_probe {

static bool parse_number(std::string_view text, uint64_t limit, uint64_t& value) {
    if (text.empty() || text.front() < '0' || text.front() > '9') return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
           value > 0 && value <= limit;
}

void print_help(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  --profile <WxH@FPS>      Target profile (default: 1280x720@30)\n"
              << "  --bitrate <RATE>         Target bitrate (default: 10M, e.g. 10M, 10000000)\n"
              << "  --duration <SECONDS>     Test duration 1..3600 seconds (default: 600)\n"
              << "  --fixture <PATH>         Path to synthetic H.264 fixture\n"
              << "  --decoder <NAME>         Force decoder element (e.g. nvv4l2decoder, openh264dec)\n"
              << "  --output <PATH>          Output JSON file destination\n"
              << "  -h, --help               Display this help message\n";
}

static bool parse_profile(ProbeOptions& options) {
    if (options.profile.size() > 64) return false;
    std::regex re(R"(^([0-9]+)x([0-9]+)@([0-9]+)$)");
    std::smatch match;
    if (!std::regex_match(options.profile, match, re)) {
        return false;
    }
    uint64_t width = 0, height = 0, rate = 0;
    if (!parse_number(match[1].str(), 16384, width) ||
        !parse_number(match[2].str(), 16384, height) ||
        !parse_number(match[3].str(), 240, rate)) return false;
    options.width = static_cast<uint32_t>(width);
    options.height = static_cast<uint32_t>(height);
    options.fps = static_cast<uint32_t>(rate);
    return true;
}

static bool parse_bitrate(const std::string& str, uint64_t& bps) {
    if (str.empty()) return false;
    std::string s = str;
    uint64_t multiplier = 1;
    char unit = s.back();
    if (unit == 'M' || unit == 'm') {
        multiplier = 1'000'000;
        s.pop_back();
    } else if (unit == 'K' || unit == 'k') {
        multiplier = 1'000;
        s.pop_back();
    }
    uint64_t val = 0;
    if (!parse_number(s, std::numeric_limits<uint64_t>::max() / multiplier, val)) return false;
    bps = val * multiplier;
    return true;
}

bool parse_options(int argc, char* argv[], ProbeOptions& opt, std::string& err) {
    std::string duration = std::to_string(opt.duration_s);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opt.help = true;
            return true;
        } else if (arg == "--profile" && i + 1 < argc) {
            opt.profile = argv[++i];
        } else if (arg.rfind("--profile=", 0) == 0) {
            opt.profile = arg.substr(10);
        } else if (arg == "--bitrate" && i + 1 < argc) {
            opt.bitrate = argv[++i];
        } else if (arg.rfind("--bitrate=", 0) == 0) {
            opt.bitrate = arg.substr(10);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration = argv[++i];
        } else if (arg.rfind("--duration=", 0) == 0) {
            duration = arg.substr(11);
        } else if (arg == "--fixture" && i + 1 < argc) {
            opt.fixture_path = argv[++i];
        } else if (arg.rfind("--fixture=", 0) == 0) {
            opt.fixture_path = arg.substr(10);
        } else if (arg == "--decoder" && i + 1 < argc) {
            opt.forced_decoder = argv[++i];
        } else if (arg.rfind("--decoder=", 0) == 0) {
            opt.forced_decoder = arg.substr(10);
        } else if (arg == "--output" && i + 1 < argc) {
            opt.output_json_path = argv[++i];
        } else if (arg.rfind("--output=", 0) == 0) {
            opt.output_json_path = arg.substr(9);
        } else {
            err = "Unknown or incomplete argument: " + arg;
            return false;
        }
    }

    if (!parse_profile(opt)) {
        err = "Malformed profile: " + opt.profile + " (expected WxH@FPS, e.g. 1280x720@30)";
        return false;
    }

    if (!parse_bitrate(opt.bitrate, opt.bitrate_bps)) {
        err = "Malformed bitrate: " + opt.bitrate + " (e.g. 10M, 10000000)";
        return false;
    }

    uint64_t seconds = 0;
    if (!parse_number(duration, 3600, seconds)) {
        err = "Duration must be an integer in 1..3600 seconds";
        return false;
    }
    opt.duration_s = static_cast<int>(seconds);

    return true;
}

} // namespace aa::decode_probe
