#include "options.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <regex>

namespace aa::decode_probe {

void print_help(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  --profile <WxH@FPS>      Target profile (default: 1280x720@30)\n"
              << "  --bitrate <RATE>         Target bitrate (default: 10M, e.g. 10M, 10000000)\n"
              << "  --duration <SECONDS>     Test duration in seconds (default: 600)\n"
              << "  --fixture <PATH>         Path to synthetic H.264 fixture\n"
              << "  --decoder <NAME>         Force decoder element (e.g. nvv4l2decoder, openh264dec)\n"
              << "  --output <PATH>          Output JSON file destination\n"
              << "  -h, --help               Display this help message\n";
}

static bool parse_profile(const std::string& str, uint32_t& w, uint32_t& h, uint32_t& fps) {
    std::regex re(R"(^([0-9]+)x([0-9]+)@([0-9]+)$)");
    std::smatch match;
    if (!std::regex_match(str, match, re)) {
        return false;
    }
    w = static_cast<uint32_t>(std::stoul(match[1]));
    h = static_cast<uint32_t>(std::stoul(match[2]));
    fps = static_cast<uint32_t>(std::stoul(match[3]));
    return (w > 0 && h > 0 && fps > 0);
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
    try {
        uint64_t val = std::stoull(s);
        bps = val * multiplier;
        return (bps > 0);
    } catch (...) {
        return false;
    }
}

bool parse_options(int argc, char* argv[], ProbeOptions& opt, std::string& err) {
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
            opt.duration_s = std::atoi(argv[++i]);
        } else if (arg.rfind("--duration=", 0) == 0) {
            opt.duration_s = std::atoi(arg.substr(11).c_str());
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

    if (!parse_profile(opt.profile, opt.width, opt.height, opt.fps)) {
        err = "Malformed profile: " + opt.profile + " (expected WxH@FPS, e.g. 1280x720@30)";
        return false;
    }

    if (!parse_bitrate(opt.bitrate, opt.bitrate_bps)) {
        err = "Malformed bitrate: " + opt.bitrate + " (e.g. 10M, 10000000)";
        return false;
    }

    if (opt.duration_s <= 0) {
        err = "Duration must be positive integer seconds";
        return false;
    }

    return true;
}

} // namespace aa::decode_probe
