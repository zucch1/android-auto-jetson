// SPDX-License-Identifier: GPL-3.0-or-later
// Task 23 consumer decode helper qualification: happy fixture decode with
// frame callbacks and monotonic A/V timestamp carry-through, typed failure on
// unsupported caps, NVIDIA backend honesty (runtime detection, recorded
// disable cause) and aa-decode-probe discovery agreement.
//
// Task-9 reduced scope (owner-signed 2026-10-06): the decode p95 latency
// budget (<=33 ms at 1280x720@30) and the software H.264 fallback
// qualification are WITHDRAWN claims. This suite records measured budgets and
// asserts NONE of them; the asserted contract is frame delivery, metadata
// carry-through and zero drops on valid fixtures.
#include <aa/consumer/Decode.hpp>

#include "discovery.hpp" // tools/aa-decode-probe: runtime detection oracle

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::consumer;
using aa::core::Milliseconds;
using aa::ErrorCode;
using aa::ipc::VideoAccessUnit;
using aa::protocol::VideoProfile;

constexpr VideoProfile kPrimaryCaps{1280, 720, 30};
constexpr VideoProfile kFallbackCaps{800, 480, 30};
constexpr std::uint64_t kTimestampBaseNs = 5'000'000'000ULL;
constexpr std::uint64_t kFramePeriodNs = 33'333'333ULL;

std::string fixture_path() {
    return std::string{AA_CONSUMER_DECODE_FIXTURE};
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        return {};
    }
    const auto size = input.tellg();
    if (size <= 0) {
        return {};
    }
    input.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(data.data()), size)) {
        return {};
    }
    return data;
}

struct AnnexBUnit final {
    std::vector<std::uint8_t> data{};
    bool idr{false};
};

// Access-unit split with the exact acceptance rules of tools/aa-decode-probe
// (src/pipeline.cpp parse_annexb_units): AUD-delimited Baseline (profile_idc
// 66) access units, multi-slice frames kept together. The probe is the
// authority for this grammar; this copy exists so the fixture feeding here
// matches what aa-decode-probe feeds.
std::vector<AnnexBUnit> parse_annexb_units(const std::vector<std::uint8_t>& stream) {
    std::vector<AnnexBUnit> units;
    std::vector<std::size_t> starts;

    for (std::size_t i = 0; i + 3 < stream.size(); ++i) {
        if (stream[i] == 0 && stream[i + 1] == 0) {
            if (stream[i + 2] == 1) {
                starts.push_back(i);
                i += 2;
            } else if (stream[i + 2] == 0 && i + 3 < stream.size() && stream[i + 3] == 1) {
                starts.push_back(i);
                i += 3;
            }
        }
    }
    if (starts.empty()) {
        return units;
    }

    std::vector<std::uint8_t> current;
    bool current_has_idr = false;
    bool au_has_slice = false;
    bool has_aud = false;

    for (std::size_t idx = 0; idx < starts.size(); ++idx) {
        const std::size_t start = starts[idx];
        const std::size_t end = (idx + 1 < starts.size()) ? starts[idx + 1] : stream.size();

        const std::size_t nal_offset = start + (stream[start + 2] == 1 ? 3 : 4);
        if (nal_offset >= stream.size()) {
            continue;
        }
        const std::uint8_t nal_type = static_cast<std::uint8_t>(stream[nal_offset] & 0x1F);
        if (nal_type == 7 && (nal_offset + 1 >= end || stream[nal_offset + 1] != 66)) {
            return {};
        }
        if (nal_type == 9) {
            has_aud = true;
        }
        if ((nal_type == 1 || nal_type == 5) && !has_aud) {
            return {};
        }

        if (nal_type == 9 || ((nal_type == 7 || nal_type == 8) && au_has_slice)) {
            if (au_has_slice) {
                units.push_back({std::move(current), current_has_idr});
                current.clear();
                current_has_idr = false;
                au_has_slice = false;
            }
        }

        current.insert(current.end(), stream.begin() + static_cast<std::ptrdiff_t>(start),
                       stream.begin() + static_cast<std::ptrdiff_t>(end));
        if (nal_type == 5) {
            current_has_idr = true;
            au_has_slice = true;
        } else if (nal_type == 1) {
            au_has_slice = true;
        }
    }
    if (au_has_slice) {
        units.push_back({std::move(current), current_has_idr});
    }
    return units;
}

double percentile_ms(std::vector<double> sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    std::sort(sorted.begin(), sorted.end());
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

// Machine-readable measured-budget record (schema aa-consumer-decode-report-1,
// same evidence pattern as the task-22 video bench report). Budgets are
// RECORDED, never asserted: the decode p95 latency budget and the software
// fallback qualification are withdrawn claims (task-9 owner signoff).
const char* report_path() {
    const char* from_env = std::getenv("AA_CONSUMER_DECODE_REPORT");
    return (from_env != nullptr && from_env[0] != '\0') ? from_env : "consumer_decode_report.json";
}

struct FixtureReport final {
    std::string selected_element;
    bool is_hardware{false};
    std::string nvidia_cause;
    std::string negotiated_caps;
    std::uint64_t frames_fed{};
    std::uint64_t frames_delivered{};
    std::uint64_t frames_unmatched{};
    std::uint64_t bytes_fed{};
    double duration_s{};
    double throughput_mbps{};
    double observed_fps{};
    std::size_t latency_samples{};
    double latency_avg_ms{};
    double latency_max_ms{};
    double latency_p95_ms{};

    struct SoftwareSection final {
        bool demonstrated{false};
        std::string selected_element;
        std::uint64_t frames_delivered{};
        std::uint64_t frames_unmatched{};
        bool metadata_identity{false};
        bool monotonic_timestamps{false};
        std::string i420_geometry;
    } software;
};

void write_fixture_report(const FixtureReport& report) {
    std::FILE* out = std::fopen(report_path(), "w");
    if (out == nullptr) {
        return;
    }
    std::fprintf(out,
                 "{\n"
                 "  \"schema\": \"aa-consumer-decode-report-1\",\n"
                 "  \"qualification\": \"consumer-side decode helper (task 23) happy fixture "
                 "decode at reduced scope; measured budgets recorded, never asserted\",\n"
                 "  \"reduced_scope_boundary\": {\n"
                 "    \"source\": \"task-9 owner signoff 2026-10-06 "
                 "(.omo/evidence/jetson-android-auto-receiver/task-9-owner-signoff.json)\",\n"
                 "    \"withdrawn_claims\": [\n"
                 "      \"decode p95 latency at most 33 ms at 1280x720@30\",\n"
                 "      \"software H.264 fallback path qualification\"\n"
                 "    ],\n"
                 "    \"budgets_asserted\": false\n"
                 "  },\n"
                 "  \"decoder\": \"%s\",\n"
                 "  \"is_hardware\": %s,\n"
                 "  \"nvidia_cause\": \"%s\",\n"
                 "  \"negotiated_caps\": \"%s\",\n"
                 "  \"frames_fed\": %llu,\n"
                 "  \"frames_delivered\": %llu,\n"
                 "  \"frames_unmatched\": %llu,\n"
                 "  \"drops\": %llu,\n"
                 "  \"bytes_fed\": %llu,\n"
                 "  \"duration_s\": %.6f,\n"
                 "  \"throughput_mbps\": %.3f,\n"
                 "  \"observed_fps\": %.3f,\n"
                 "  \"latency_recorded_not_asserted\": {\n"
                 "    \"samples\": %zu,\n"
                 "    \"avg_ms\": %.3f,\n"
                 "    \"max_ms\": %.3f,\n"
                 "    \"p95_ms\": %.3f\n"
                 "  },\n"
                 "  \"timestamp_correlation\": {\n"
                 "    \"source\": \"task-22 channel framing metadata (frame, sequence, "
                 "timestamp_ns)\",\n"
                 "    \"carried_through\": true,\n"
                 "    \"clock\": \"monotonic\"\n"
                 "  },\n"
                 "  \"software_backend_fixture\": {\n"
                 "    \"demonstrated\": %s,\n"
                 "    \"selected_element\": \"%s\",\n"
                 "    \"frames_delivered\": %llu,\n"
                 "    \"frames_unmatched\": %llu,\n"
                 "    \"metadata_identity\": %s,\n"
                 "    \"monotonic_timestamps\": %s,\n"
                 "    \"i420_geometry\": \"%s\",\n"
                 "    \"no_latency_claim\": true,\n"
                 "    \"no_fallback_qualification_claim\": true\n"
                 "  }\n"
                 "}\n",
                 report.selected_element.c_str(), report.is_hardware ? "true" : "false",
                 report.nvidia_cause.c_str(), report.negotiated_caps.c_str(),
                 static_cast<unsigned long long>(report.frames_fed),
                 static_cast<unsigned long long>(report.frames_delivered),
                 static_cast<unsigned long long>(report.frames_unmatched),
                 static_cast<unsigned long long>(report.frames_fed - report.frames_delivered),
                 static_cast<unsigned long long>(report.bytes_fed), report.duration_s,
                 report.throughput_mbps, report.observed_fps, report.latency_samples,
                 report.latency_avg_ms, report.latency_max_ms, report.latency_p95_ms,
                 report.software.demonstrated ? "true" : "false",
                 report.software.selected_element.c_str(),
                 static_cast<unsigned long long>(report.software.frames_delivered),
                 static_cast<unsigned long long>(report.software.frames_unmatched),
                 report.software.metadata_identity ? "true" : "false",
                 report.software.monotonic_timestamps ? "true" : "false",
                 report.software.i420_geometry.c_str());
    std::fclose(out);
}

std::string read_stream(FILE* pipe) {
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }
    return output;
}

} // namespace

// ---------------------------------------------------------------------------
// Caps negotiation (QA failure scenario lives here: unsupported caps must fail
// negotiation with a typed error, never a crash and never wrong dimensions).
// ---------------------------------------------------------------------------

TEST(ConsumerDecodeCaps, PrimaryProfileNegotiates) {
    DecodeConfig config;
    config.caps = kPrimaryCaps;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_TRUE(helper) << static_cast<int>(helper.error().code());
    EXPECT_EQ(helper.value().report().selected_element.empty(), false);
    ASSERT_FALSE(helper.value().report().nvidia_cause.empty());
    const std::string& cause = helper.value().report().nvidia_cause;
    EXPECT_TRUE(cause.rfind("enabled:", 0) == 0 || cause.rfind("disabled:", 0) == 0) << cause;
    helper.value().close();
    EXPECT_TRUE(helper.value().closed());
    helper.value().close(); // idempotent
}

TEST(ConsumerDecodeCaps, FallbackProfileNegotiates) {
    DecodeConfig config;
    config.caps = kFallbackCaps;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_TRUE(helper) << static_cast<int>(helper.error().code());
    EXPECT_FALSE(helper.value().report().selected_element.empty());
    helper.value().close();
    EXPECT_TRUE(helper.value().closed());
}

TEST(ConsumerDecodeCaps, UnsupportedDimensionsFailTyped) {
    DecodeConfig config;
    config.caps = VideoProfile{1920, 1080, 30};
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_FALSE(helper);
    EXPECT_EQ(helper.error().code(), ErrorCode::channel_caps_unsupported);
}

TEST(ConsumerDecodeCaps, UnsupportedFrameRateFailsTyped) {
    DecodeConfig config;
    config.caps = VideoProfile{1280, 720, 60};
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_FALSE(helper);
    EXPECT_EQ(helper.error().code(), ErrorCode::channel_caps_unsupported);
}

TEST(ConsumerDecodeCaps, EmptyCapsFailTyped) {
    DecodeConfig config;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_FALSE(helper);
    EXPECT_EQ(helper.error().code(), ErrorCode::channel_caps_unsupported);
}

TEST(ConsumerDecodeCaps, PostCloseOperationsFailTyped) {
    DecodeConfig config;
    config.caps = kPrimaryCaps;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_TRUE(helper);
    helper.value().close();
    const auto submit = helper.value().submit(
        VideoAccessUnit{0, 0, kTimestampBaseNs, true, std::vector<std::uint8_t>{0, 0, 1, 0x65}});
    ASSERT_FALSE(submit);
    EXPECT_EQ(submit.error().code(), ErrorCode::invalid_argument);
    const auto pump = helper.value().pump(Milliseconds{1});
    ASSERT_FALSE(pump);
    EXPECT_EQ(pump.error().code(), ErrorCode::invalid_argument);
}

// ---------------------------------------------------------------------------
// Happy fixture decode: a valid fixture stream is decoded and delivered via
// callback with monotonic timestamps carrying the channel's
// frame/sequence/timestamp_ns metadata through unchanged. Run once on the
// automatic backend selection and once explicitly on DecodeBackend::software
// (mandatory functional software-backend coverage; gate fix F2).
// ---------------------------------------------------------------------------

struct SentMeta final {
    std::uint64_t frame{};
    std::uint64_t sequence{};
    std::uint64_t timestamp_ns{};
};

struct FixtureRun final {
    std::string failure;
    std::vector<SentMeta> sent;
    std::vector<DecodedFrame> frames;
    std::vector<double> latencies_ms;
    std::uint64_t bytes_fed{};
    double duration_s{};
    std::string selected_element;
    bool selected_hardware{};
    std::string nvidia_cause;
    std::string negotiated_caps;
    std::uint64_t unmatched{};
};

FixtureRun run_fixture_decode(DecodeBackend backend) {
    FixtureRun run;
    const auto stream = read_file(fixture_path());
    if (stream.empty()) {
        run.failure = "fixture missing: " + fixture_path();
        return run;
    }
    const auto units = parse_annexb_units(stream);
    if (units.size() != 90U) {
        run.failure = "fixture grammar drift";
        return run;
    }

    std::vector<std::chrono::steady_clock::time_point> sent_at;
    DecodeConfig config;
    config.caps = kPrimaryCaps;
    config.backend = backend;
    config.on_frame = [&](const DecodedFrame& frame) {
        run.latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                      - sent_at[frame.frame])
                .count());
        run.frames.push_back(frame);
    };
    auto helper = DecodeHelper::open(std::move(config));
    if (!helper) {
        run.failure = "open failed, code " + std::to_string(static_cast<int>(helper.error().code()));
        return run;
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < units.size(); ++i) {
        const std::uint64_t frame_id = static_cast<std::uint64_t>(i);
        const std::uint64_t sequence = 3 * frame_id + 1;
        const std::uint64_t timestamp_ns = kTimestampBaseNs + frame_id * kFramePeriodNs;
        run.sent.push_back(SentMeta{frame_id, sequence, timestamp_ns});
        sent_at.push_back(std::chrono::steady_clock::now());
        run.bytes_fed += units[i].data.size();
        const auto result = helper.value().submit(
            VideoAccessUnit{frame_id, sequence, timestamp_ns, units[i].idr, units[i].data});
        if (!result) {
            run.failure = "submit failed at frame " + std::to_string(frame_id) + ", code "
                + std::to_string(static_cast<int>(result.error().code()));
            return run;
        }
    }
    if (const auto eos = helper.value().end_of_stream(); !eos) {
        run.failure = "end_of_stream failed, code "
            + std::to_string(static_cast<int>(eos.error().code()));
        return run;
    }

    std::size_t delivered = 0;
    for (int poll = 0; poll < 200 && delivered < units.size(); ++poll) {
        const auto drained = helper.value().pump(Milliseconds{50});
        if (!drained) {
            run.failure = "pump failed, code "
                + std::to_string(static_cast<int>(drained.error().code()));
            return run;
        }
        delivered += drained.value();
    }
    run.duration_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    run.selected_element = helper.value().report().selected_element;
    run.selected_hardware = helper.value().report().nvidia_enabled;
    run.nvidia_cause = helper.value().report().nvidia_cause;
    run.negotiated_caps = helper.value().report().negotiated_caps;
    run.unmatched = helper.value().report().frames_unmatched;
    helper.value().close();
    return run;
}

void expect_fixture_delivery(const FixtureRun& run) {
    ASSERT_EQ(run.frames.size(), run.sent.size());
    EXPECT_EQ(run.unmatched, 0U);
    for (std::size_t i = 0; i < run.frames.size(); ++i) {
        EXPECT_EQ(run.frames[i].frame, run.sent[i].frame);
        EXPECT_EQ(run.frames[i].sequence, run.sent[i].sequence);
        EXPECT_EQ(run.frames[i].timestamp_ns, run.sent[i].timestamp_ns);
        if (i > 0) {
            EXPECT_LT(run.frames[i - 1].timestamp_ns, run.frames[i].timestamp_ns);
        }
        EXPECT_EQ(run.frames[i].width, 1280);
        EXPECT_EQ(run.frames[i].height, 720);
        EXPECT_EQ(run.frames[i].i420.size(), 1280U * 720U * 3U / 2U);
    }
}

FixtureReport fixture_report;

TEST(ConsumerDecodeFixture, HappyFixtureDecodeDeliversMonotonicFrames) {
    const FixtureRun run = run_fixture_decode(DecodeBackend::automatic);
    ASSERT_TRUE(run.failure.empty()) << run.failure;
    ASSERT_EQ(run.frames.size(), 90U);
    expect_fixture_delivery(run);

    fixture_report.selected_element = run.selected_element;
    fixture_report.is_hardware = run.selected_hardware;
    fixture_report.nvidia_cause = run.nvidia_cause;
    fixture_report.negotiated_caps = run.negotiated_caps;
    fixture_report.frames_fed = run.sent.size();
    fixture_report.frames_delivered = run.frames.size();
    fixture_report.frames_unmatched = run.unmatched;
    fixture_report.bytes_fed = run.bytes_fed;
    fixture_report.duration_s = run.duration_s;
    fixture_report.throughput_mbps =
        static_cast<double>(run.bytes_fed) * 8.0 / std::max(run.duration_s, 1e-9) / 1e6;
    fixture_report.observed_fps =
        static_cast<double>(run.frames.size()) / std::max(run.duration_s, 1e-9);
    fixture_report.latency_samples = run.latencies_ms.size();
    if (!run.latencies_ms.empty()) {
        double sum = 0.0;
        for (const double sample : run.latencies_ms) {
            sum += sample;
        }
        fixture_report.latency_avg_ms = sum / static_cast<double>(run.latencies_ms.size());
        fixture_report.latency_max_ms = *std::max_element(run.latencies_ms.begin(), run.latencies_ms.end());
        fixture_report.latency_p95_ms = percentile_ms(run.latencies_ms, 0.95);
    }
    // Latency correlation completeness (identity of samples, not a budget).
    EXPECT_EQ(fixture_report.latency_samples, run.frames.size());
    write_fixture_report(fixture_report);
}

TEST(ConsumerDecodeFixture, ForcedSoftwareBackendDeliversFixtureFrames) {
    // Mandatory functional software-backend coverage (gate fix F2): the suite
    // must demonstrate frame delivery through an explicitly software-selected
    // backend. NO latency claim and NO software-fallback QUALIFICATION claim
    // is made here (the latter is a withdrawn claim); this is functional
    // coverage only.
    const FixtureRun run = run_fixture_decode(DecodeBackend::software);
    ASSERT_TRUE(run.failure.empty()) << run.failure;

    EXPECT_FALSE(run.selected_hardware);
    const DecodeReport probe = DecodeHelper::probe_backends();
    const auto selected =
        std::find_if(probe.backends.begin(), probe.backends.end(),
                     [&](const BackendCandidate& c) { return c.element == run.selected_element; });
    ASSERT_NE(selected, probe.backends.end());
    EXPECT_FALSE(selected->hardware);
    EXPECT_TRUE(selected->viable);

    ASSERT_EQ(run.frames.size(), 90U);
    expect_fixture_delivery(run);

    fixture_report.software.demonstrated = true;
    fixture_report.software.selected_element = run.selected_element;
    fixture_report.software.frames_delivered = run.frames.size();
    fixture_report.software.frames_unmatched = run.unmatched;
    fixture_report.software.metadata_identity = true;
    fixture_report.software.monotonic_timestamps = true;
    fixture_report.software.i420_geometry = "1280x720";
    write_fixture_report(fixture_report);
}

TEST(ConsumerDecodeFixture, MalformedAccessUnitFailsTypedWithoutCrash) {
    DecodeConfig config;
    config.caps = kPrimaryCaps;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_TRUE(helper);

    const std::vector<std::uint8_t> garbage{'N', 'O', 'T', '_', 'A', '_', 'V', 'A', 'L', 'I', 'D'};
    bool typed_failure = false;
    ErrorCode observed = ErrorCode::internal;

    const auto submitted =
        helper.value().submit(VideoAccessUnit{0, 0, kTimestampBaseNs, true, garbage});
    if (!submitted) {
        typed_failure = true;
        observed = submitted.error().code();
    } else {
        const auto eos = helper.value().end_of_stream();
        if (!eos) {
            typed_failure = true;
            observed = eos.error().code();
        } else {
            const auto pumped = helper.value().pump(Milliseconds{2000});
            if (!pumped) {
                typed_failure = true;
                observed = pumped.error().code();
            }
        }
    }

    EXPECT_TRUE(typed_failure) << "malformed access unit must fail typed, not silently";
    EXPECT_EQ(observed, ErrorCode::channel_decode_failed);
    helper.value().close();
    EXPECT_TRUE(helper.value().closed());
}

// ---------------------------------------------------------------------------
// Backend honesty: NVIDIA behind runtime detection, explicit disable with
// recorded cause when absent (the aa-decode-probe honesty pattern).
// ---------------------------------------------------------------------------

TEST(ConsumerDecodeBackend, NvidiaPathIsEnabledOrRecordedDisabled) {
    const DecodeReport probe = DecodeHelper::probe_backends();
    const auto nvidia = std::find_if(probe.backends.begin(), probe.backends.end(),
                                     [](const BackendCandidate& c) { return c.hardware; });
    ASSERT_NE(nvidia, probe.backends.end());
    EXPECT_EQ(nvidia->element, "nvv4l2decoder");
    ASSERT_FALSE(probe.nvidia_cause.empty());

    const bool nvidia_viable =
        std::any_of(probe.backends.begin(), probe.backends.end(),
                    [](const BackendCandidate& c) { return c.hardware && c.viable; });

    DecodeConfig config;
    config.caps = kPrimaryCaps;
    config.backend = DecodeBackend::nvidia;
    auto helper = DecodeHelper::open(std::move(config));
    if (nvidia_viable) {
        ASSERT_TRUE(helper) << static_cast<int>(helper.error().code());
        EXPECT_TRUE(helper.value().report().nvidia_enabled);
        // Selection policy (probe mirror): first viable hardware candidate in
        // the known table, which is nvv4l2decoder when present and nvh264dec
        // otherwise - never an assumed Jetson element.
        const auto selected =
            std::find_if(probe.backends.begin(), probe.backends.end(),
                         [&](const BackendCandidate& c) {
                             return c.element == helper.value().report().selected_element;
                         });
        ASSERT_NE(selected, probe.backends.end());
        EXPECT_TRUE(selected->hardware);
        EXPECT_TRUE(selected->viable);
        helper.value().close();
    } else {
        ASSERT_FALSE(helper);
        EXPECT_EQ(helper.error().code(), ErrorCode::channel_decode_unavailable);
        // Explicit disable with recorded cause, never a silent fallback claim.
        EXPECT_EQ(probe.nvidia_cause.rfind("disabled:", 0), 0U) << probe.nvidia_cause;
    }
}

TEST(ConsumerDecodeBackend, SoftwareRequestRecordsNvidiaDisableCause) {
    const DecodeReport probe = DecodeHelper::probe_backends();
    const bool software_viable =
        std::any_of(probe.backends.begin(), probe.backends.end(),
                    [](const BackendCandidate& c) { return !c.hardware && c.viable; });
    if (!software_viable) {
        GTEST_SKIP() << "no viable host software H.264 decoder on this host";
    }
    DecodeConfig config;
    config.caps = kPrimaryCaps;
    config.backend = DecodeBackend::software;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_TRUE(helper) << static_cast<int>(helper.error().code());
    EXPECT_FALSE(helper.value().report().nvidia_enabled);
    EXPECT_EQ(helper.value().report().nvidia_cause.rfind("disabled:", 0), 0U)
        << helper.value().report().nvidia_cause;
    for (const auto& candidate : helper.value().report().backends) {
        if (candidate.element == helper.value().report().selected_element) {
            EXPECT_FALSE(candidate.hardware);
        }
    }
    helper.value().close();
}

TEST(ConsumerDecodeBackend, ProbeBackendsReportsEveryKnownElement) {
    const DecodeReport probe = DecodeHelper::probe_backends();
    ASSERT_EQ(probe.backends.size(), 4U);
    for (const auto& candidate : probe.backends) {
        EXPECT_FALSE(candidate.element.empty());
        EXPECT_FALSE(candidate.cause.empty()) << candidate.element;
        if (candidate.requires_nvmm_conversion && candidate.viable) {
            EXPECT_TRUE(probe.nvmm_conversion_available) << candidate.element;
        }
    }
    EXPECT_FALSE(probe.nvmm_conversion_cause.empty());
    const bool found_nvidia = std::any_of(
        probe.backends.begin(), probe.backends.end(),
        [](const BackendCandidate& c) { return c.element == "nvv4l2decoder"; });
    EXPECT_TRUE(found_nvidia) << "nvv4l2decoder must be reported, never assumed";
}

TEST(ConsumerDecodeBackend, NvmmConversionGatesNvidiaPath) {
    // Synthetic Jetson-shaped table: nvv4l2decoder is READY-viable but emits
    // only video/x-raw(memory:NVMM) while the NVMM conversion (nvvidconv) is
    // absent - the exact output-path gap the policy must close.
    BackendCandidate nv;
    nv.element = "nvv4l2decoder";
    nv.hardware = true;
    nv.requires_nvmm_conversion = true;
    nv.available = true;
    nv.viable = true;
    nv.cause = "Instantiated and transitioned to READY successfully";
    apply_output_path_policy(nv, false);
    EXPECT_FALSE(nv.viable);
    EXPECT_NE(nv.cause.find("nvvidconv"), std::string::npos) << nv.cause;

    BackendCandidate software;
    software.element = "openh264dec";
    software.available = true;
    software.viable = true;
    software.cause = "Instantiated and transitioned to READY successfully";
    apply_output_path_policy(software, false);
    EXPECT_TRUE(software.viable);

    DecodeReport incomplete;
    incomplete.backends = {nv, software};
    incomplete.nvmm_conversion_available = false;

    const auto automatic = select_backend(incomplete, DecodeBackend::automatic);
    EXPECT_TRUE(automatic.usable);
    EXPECT_EQ(automatic.element, "openh264dec");
    EXPECT_FALSE(automatic.nvidia_enabled);
    EXPECT_EQ(automatic.nvidia_cause.rfind("disabled:", 0), 0U) << automatic.nvidia_cause;

    // Explicit NVIDIA request without the complete path must be unusable,
    // which open() maps to channel_decode_unavailable: no silent downgrade.
    const auto explicit_nvidia = select_backend(incomplete, DecodeBackend::nvidia);
    EXPECT_FALSE(explicit_nvidia.usable);
    EXPECT_EQ(explicit_nvidia.nvidia_cause.rfind("disabled:", 0), 0U)
        << explicit_nvidia.nvidia_cause;

    // With the conversion present the same candidate becomes usable and is
    // preferred again.
    BackendCandidate nv_with_conversion;
    nv_with_conversion.element = "nvv4l2decoder";
    nv_with_conversion.hardware = true;
    nv_with_conversion.requires_nvmm_conversion = true;
    nv_with_conversion.available = true;
    nv_with_conversion.viable = true;
    nv_with_conversion.cause = "Instantiated and transitioned to READY successfully";
    apply_output_path_policy(nv_with_conversion, true);
    EXPECT_TRUE(nv_with_conversion.viable);
    DecodeReport complete;
    complete.backends = {nv_with_conversion, software};
    complete.nvmm_conversion_available = true;
    const auto preferred = select_backend(complete, DecodeBackend::automatic);
    EXPECT_TRUE(preferred.usable);
    EXPECT_EQ(preferred.element, "nvv4l2decoder");
    EXPECT_TRUE(preferred.nvidia_enabled);
}

// ---------------------------------------------------------------------------
// aa-decode-probe integration: the helper's runtime detection agrees with the
// probe's discovery (same table, same viability rule, same selection policy)
// and the probe binary smoke-reports the same decoder for the same fixture.
// ---------------------------------------------------------------------------

TEST(ConsumerProbeIntegration, DiscoveryAgreesWithDecodeProbe) {
    // probe_backends() initializes GStreamer; run it before the probe's own
    // discovery so both observe the same registry state.
    const DecodeReport ours = DecodeHelper::probe_backends();
    const aa::decode_probe::DiscoveryResult theirs = aa::decode_probe::discover_decoders("");

    ASSERT_EQ(ours.backends.size(), theirs.candidates.size());
    for (std::size_t i = 0; i < ours.backends.size(); ++i) {
        EXPECT_EQ(ours.backends[i].element, theirs.candidates[i].element_name);
        EXPECT_EQ(ours.backends[i].hardware, theirs.candidates[i].is_hardware);
        EXPECT_EQ(ours.backends[i].available, theirs.candidates[i].available);
        // The probe reports decoder-level READY viability; ours is the
        // complete-output-path viability (decoder + NVMM conversion rule), so
        // the probe's verdict must equal ours after the same policy is applied.
        BackendCandidate expected = ours.backends[i];
        expected.viable = theirs.candidates[i].viable;
        apply_output_path_policy(expected, ours.nvmm_conversion_available);
        EXPECT_EQ(ours.backends[i].viable, expected.viable);
    }

    DecodeConfig config;
    config.caps = kPrimaryCaps;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_TRUE(helper) << static_cast<int>(helper.error().code());
    const auto theirs_pick =
        std::find_if(ours.backends.begin(), ours.backends.end(),
                     [&](const BackendCandidate& c) { return c.element == theirs.selected_decoder; });
    ASSERT_NE(theirs_pick, ours.backends.end());
    if (theirs_pick->viable) {
        EXPECT_EQ(helper.value().report().selected_element, theirs.selected_decoder);
        EXPECT_EQ(helper.value().report().nvidia_enabled, theirs.is_hardware_selected);
    } else {
        EXPECT_FALSE(helper.value().report().nvidia_enabled);
        EXPECT_NE(helper.value().report().selected_element, theirs.selected_decoder);
    }
    const std::string selected = helper.value().report().selected_element;
    helper.value().close();

    // Tool-level smoke: the probe binary auto-discovery reports the same
    // decoder for the same fixture (no --decoder flag, so the full
    // discovered_elements table including nvv4l2decoder is reported). No probe
    // budget is asserted here (task-9 reduced scope).
    const std::string command = std::string("\"") + AA_CONSUMER_DECODE_PROBE
        + "\" --profile 1280x720@30 --bitrate 10M"
        + " --duration 1 --fixture \"" + fixture_path() + "\"";
    FILE* pipe = popen(command.c_str(), "r");
    ASSERT_NE(pipe, nullptr);
    const std::string output = read_stream(pipe);
    const int status = pclose(pipe);
    EXPECT_TRUE(WIFEXITED(status)) << "aa-decode-probe crashed";
    EXPECT_NE(output.find("\"decoder\""), std::string::npos) << output;
    EXPECT_NE(output.find(selected), std::string::npos) << output;
    EXPECT_NE(output.find("nvv4l2decoder"), std::string::npos)
        << "probe discovery must always report nvv4l2decoder: " << output;
}
