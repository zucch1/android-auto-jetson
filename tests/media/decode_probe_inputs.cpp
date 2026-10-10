// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_report.hpp"
#include "options.hpp"
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace aa::decode_probe;
    int failures = 0;
    // Given malformed and overflowing numeric inputs, when parsed, then reject without throwing.
    const std::vector<std::pair<std::string, std::string>> invalid = {
        {"--profile", "184467440737095516160x720@30"},
        {"--profile", "4294968576x720@30"}, {"--profile", "1280x720@0"},
        {"--profile", "1280x720@1000001"}, {"--profile", "1280x720@30junk"},
        {"--bitrate", "10junk"}, {"--bitrate", "-1"}, {"--bitrate", "+10"},
        {"--bitrate", "18446744073709551615M"}, {"--bitrate", " 10M"},
        {"--duration", "1junk"}, {"--duration", "4294967297"},
        {"--duration", "2147483647"}, {"--duration", "0"}, {"--duration", "-1"}
    };
    for (const auto& [flag, value] : invalid) {
        std::vector<std::string> args = {"probe", flag, value};
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args) argv.push_back(arg.data());
        ProbeOptions options;
        std::string error;
        try {
            if (parse_options(static_cast<int>(argv.size()), argv.data(), options, error)) {
                std::cerr << "FAIL accepted " << flag << " " << value << '\n';
                ++failures;
            }
        } catch (const std::exception& e) {
            std::cerr << "FAIL threw " << flag << ": " << e.what() << '\n';
            ++failures;
        }
    }
    // Given strings containing JSON syntax/control bytes, when reported, then escape every field.
    const std::string hostile = "quote\"slash\\line\n\t\r\b\f\x01";
    const std::string escaped = "quote\\\"slash\\\\line\\n\\t\\r\\b\\f\\u0001";
    ProbeOptions options;
    options.profile = hostile;
    options.bitrate = hostile;
    DiscoveryResult discovery;
    DecoderCandidate candidate;
    candidate.element_name = hostile;
    candidate.notes = hostile;
    discovery.candidates.push_back(candidate);
    ProbeMetrics metrics;
    metrics.decoder_used = hostile;
    metrics.negotiated_caps_str = hostile;
    metrics.failure_reason = hostile;
    const auto report = format_json_report(options, discovery, metrics);
    for (const auto* field : {"profile", "bitrate", "decoder", "caps_negotiated", "failure_reason", "name", "notes"}) {
        if (report.find(std::string("\"") + field + "\": \"" + escaped + "\"") == std::string::npos) {
            std::cerr << "FAIL JSON escaping field " << field << '\n';
            ++failures;
        }
    }
    std::cout << "input/JSON failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
