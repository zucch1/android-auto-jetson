// SPDX-License-Identifier: GPL-3.0-or-later
// Manual driver for the task-20 replay QA gate. Emits the golden synthetic
// fixture, deliberately-bad fixtures for every gate class, and runs the
// authoritative accept/reject check. Independently rerunnable:
//   aa_replay_fixture_probe emit-good /tmp/f.json
//   aa_replay_fixture_probe emit-bad raw-identifier /tmp/bad.json
//   aa_replay_fixture_probe check /tmp/f.json

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>
#include <aa/replay/Schema.hpp>

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace replaytest;
namespace replay = aa::replay;

std::vector<std::byte> golden_file() {
    replay::Recorder recorder;
    fill_golden(recorder);
    auto exported = recorder.export_fixture();
    if (!exported) {
        std::cerr << "emit failed: " << aa::to_string(exported.error().code()) << "\n";
        return {};
    }
    return exported.value().file_bytes;
}

std::string discovery_hex() { return hex_of(discovery_request_frame()); }

using Transform = std::optional<std::vector<std::byte>> (*)(std::span<const std::byte>);

std::optional<std::vector<std::byte>> raw_identifier(std::span<const std::byte> file) {
    return tamper(file, R"(\"frames\":19)", R"(\"frames\":123456789012345)");
}

std::optional<std::vector<std::byte>> raw_mac(std::span<const std::byte> file) {
    return tamper(file, R"(\"phone_id\":\"[redacted]\")",
                  R"(\"phone_id\":\"AA:BB:CC:DD:EE:FF\")");
}

std::optional<std::vector<std::byte>> secret_key(std::span<const std::byte> file) {
    return tamper(file, R"(\"codec\":\"h264\")",
                  R"(\"codec\":\"h264\",\"api_key\":\"hunter2\")");
}

std::optional<std::vector<std::byte>> opaque_policy(std::span<const std::byte> file) {
    const auto from = R"("opaque":"synthetic","frame":")" + discovery_hex() + R"(")";
    return tamper(file, from, R"("opaque":"raw","frame":")" + discovery_hex() + R"(")");
}

std::optional<std::vector<std::byte>> missing_opaque(std::span<const std::byte> file) {
    const auto from = R"("opaque":"synthetic","frame":")" + discovery_hex() + R"(")";
    return tamper(file, from, R"("frame":")" + discovery_hex() + R"(")");
}

std::optional<std::vector<std::byte>> corrupt_hash(std::span<const std::byte> file) {
    return flip_hash_char(file);
}

std::optional<std::vector<std::byte>> wrong_schema(std::span<const std::byte> file) {
    return tamper(file, R"("aa-replay-fixture-v1")", R"("aa-replay-fixture-v9")");
}

std::optional<std::vector<std::byte>> bad_record(std::span<const std::byte> file) {
    return tamper(file, R"("kind":"audio")", R"("kind":"bogus")");
}

std::optional<std::vector<std::byte>> oversize(std::span<const std::byte> file) {
    const auto from = R"("frame":")" + discovery_hex() + R"(")";
    return tamper(file, from, R"("frame":")" + std::string(40'000, 'a') + R"(")");
}

std::optional<std::vector<std::byte>> nonmonotonic(std::span<const std::byte> file) {
    return tamper(file, R"("t":12000000)", R"("t":1)");
}

std::optional<std::vector<std::byte>> illegal_state(std::span<const std::byte> file) {
    return tamper(file,
                  R"("states":["disconnected","discovering","connecting","negotiating","active"])",
                  R"("states":["disconnected","active"])");
}

std::optional<std::vector<std::byte>> inconsistent_metadata(std::span<const std::byte> file) {
    return tamper(file, R"(\"frames\":19)", R"(\"frames\":199)");
}

std::optional<std::vector<std::byte>> inconsistent_id(std::span<const std::byte> file) {
    return tamper(file, R"("id":100,)", R"("id":99,)");
}

std::optional<std::vector<std::byte>> wrong_expect(std::span<const std::byte> file) {
    return tamper(file,
                  R"("states":["disconnected","discovering","connecting","negotiating","active"])",
                  R"("states":["disconnected","discovering","connecting","negotiating"])");
}

const std::map<std::string, Transform>& bad_cases() {
    static const std::map<std::string, Transform> cases{
        {"raw-identifier", &raw_identifier},
        {"raw-mac", &raw_mac},
        {"secret-key", &secret_key},
        {"opaque-policy", &opaque_policy},
        {"missing-opaque", &missing_opaque},
        {"corrupt-hash", &corrupt_hash},
        {"wrong-schema", &wrong_schema},
        {"bad-record", &bad_record},
        {"oversize", &oversize},
        {"nonmonotonic", &nonmonotonic},
        {"illegal-state", &illegal_state},
        {"inconsistent-metadata", &inconsistent_metadata},
        {"inconsistent-id", &inconsistent_id},
        {"wrong-expect", &wrong_expect},
    };
    return cases;
}

bool write_file(const std::string& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "cannot open " << path << "\n";
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// Bounded read: never buffers more than the loader accepts, so an oversized
// input is rejected with the same typed error instead of being slurped whole.
aa::core::Result<std::vector<std::byte>> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    std::string text(replay::kMaxFixtureBytes + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto got = in.gcount();
    if (got > static_cast<std::streamsize>(replay::kMaxFixtureBytes)) {
        return aa::Error{aa::ErrorCode::transport_oversize_frame};
    }
    text.resize(static_cast<std::size_t>(got));
    return to_bytes(text);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string{argv[1]} == "emit-good") {
        const auto file = golden_file();
        return (file.empty() || !write_file(argv[2], file)) ? 1 : 0;
    }
    if (argc == 4 && std::string{argv[1]} == "emit-bad") {
        const auto found = bad_cases().find(argv[2]);
        if (found == bad_cases().end()) {
            std::cerr << "unknown case " << argv[2] << "\n";
            return 2;
        }
        const auto good = golden_file();
        auto bad = found->second(good);
        if (!bad) {
            std::cerr << "tamper target missing for " << argv[2] << "\n";
            return 1;
        }
        if (std::string{argv[2]} != "corrupt-hash") {
            bad = reseal(*bad);
            if (!bad) {
                std::cerr << "reseal failed for " << argv[2] << "\n";
                return 1;
            }
        }
        return write_file(argv[3], *bad) ? 0 : 1;
    }
    if (argc == 4 && std::string{argv[1]} == "reseal") {
        const auto file = read_file(argv[2]);
        if (!file) {
            std::cerr << "read rejected:" << aa::to_string(file.error().code()) << "\n";
            return 1;
        }
        const auto sealed = reseal(file.value());
        if (!sealed) {
            std::cerr << "reseal failed\n";
            return 1;
        }
        return write_file(argv[3], *sealed) ? 0 : 1;
    }
    if (argc == 3 && std::string{argv[1]} == "run") {
        const auto file = read_file(argv[2]);
        if (!file) {
            std::cout << "load-rejected:" << aa::to_string(file.error().code()) << "\n";
            return 1;
        }
        const auto loaded = replay::load_fixture(file.value());
        if (!loaded) {
            std::cout << "load-rejected:" << aa::to_string(loaded.error().code()) << "\n";
            return 1;
        }
        MapTrustStore trust;
        replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
        RecordingSink video_sink;
        RecordingSink input_sink;
        AudioCapture audio;
        (void)player.register_sink(proto::ChannelRole::video, video_sink);
        (void)player.register_sink(proto::ChannelRole::input, input_sink);
        (void)player.register_audio_sink(audio);
        const auto observed = player.run();
        if (!observed) {
            std::cout << "run-rejected:" << aa::to_string(observed.error().code()) << "\n";
            return 1;
        }
        std::cout << "ran:" << observed.value().states.size() << "\n";
        return 0;
    }
    if (argc == 3 && std::string{argv[1]} == "check") {
        const auto file = read_file(argv[2]);
        if (!file) {
            std::cout << "rejected:" << aa::to_string(file.error().code()) << "\n";
            return 1;
        }
        const auto verdict = replay::check_fixture(file.value());
        if (verdict) {
            std::cout << "accepted\n";
            return 0;
        }
        std::cout << "rejected:" << aa::to_string(verdict.error().code()) << "\n";
        return 1;
    }
    std::cerr << "usage: aa_replay_fixture_probe emit-good <path>\n"
                 "       aa_replay_fixture_probe emit-bad <case> <path>\n"
                 "       aa_replay_fixture_probe check <path>\n"
                 "       aa_replay_fixture_probe run <path>\n"
                 "       aa_replay_fixture_probe reseal <in> <out>\n"
                 "cases:";
    for (const auto& entry : bad_cases()) {
        std::cerr << " " << entry.first;
    }
    std::cerr << "\n";
    return 2;
}
