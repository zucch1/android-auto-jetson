// SPDX-License-Identifier: GPL-3.0-or-later
// Task 23 consumer decode helper qualification: happy fixture decode with
// frame callbacks and monotonic A/V timestamp carry-through, typed failure on
// unsupported caps, NVIDIA backend honesty (runtime detection, recorded
// disable cause), aa-decode-probe discovery agreement, and the session
// admission contracts (feed-backlog math + timestamp high-water mark).
//
// Task-9 reduced scope (owner-signed 2026-10-06): the decode p95 latency
// budget (<=33 ms at 1280x720@30) and the software H.264 fallback
// qualification are WITHDRAWN claims. This suite records measured budgets and
// asserts NONE of them; the asserted contract is frame delivery, metadata
// carry-through and zero drops on valid fixtures.
#include <aa/consumer/Decode.hpp>

#include "DecodeSessionState.hpp" // GStreamer-free admission + timestamp state
#include "discovery.hpp"          // tools/aa-decode-probe: runtime detection oracle

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

// ---------------------------------------------------------------------------
// Session admission contracts (PR #47 fix round findings 3+4): feed-backlog
// admission counts the incoming AU (subtraction-based, overflow-free) and the
// session's timestamps are valid, strictly increasing and session-unique
// behind a high-water mark that advances only after a successful push. The
// queue-level cases drive the GStreamer-free DecodeSessionState helper
// directly so the admission math is deterministic and never races appsrc.
// ---------------------------------------------------------------------------

TEST(ConsumerDecodeAdmission, OversizedAuRejectedWithEmptyQueue) {
    DecodeSessionState state;
    const std::size_t oversized =
        static_cast<std::size_t>(DecodeSessionState::kMaxFeedBacklogBytes) + 1U;
    const auto admitted = state.admit(FeedLoad{0, oversized}, kTimestampBaseNs);
    ASSERT_FALSE(admitted);
    EXPECT_EQ(admitted.error().code(), ErrorCode::ipc_queue_full);
    EXPECT_FALSE(state.last_accepted_timestamp_ns.has_value());
}

TEST(ConsumerDecodeAdmission, ExactRemainingCapacityAccepted) {
    const std::uint64_t limit = DecodeSessionState::kMaxFeedBacklogBytes;
    DecodeSessionState at_capacity;
    EXPECT_TRUE(at_capacity.admit(FeedLoad{limit - 1024, 1024}, kTimestampBaseNs));
    DecodeSessionState empty;
    EXPECT_TRUE(empty.admit(FeedLoad{0, static_cast<std::size_t>(limit)}, kTimestampBaseNs));
}

TEST(ConsumerDecodeAdmission, OneBytePastRemainingCapacityRejected) {
    const std::uint64_t limit = DecodeSessionState::kMaxFeedBacklogBytes;
    DecodeSessionState state;
    const auto admitted = state.admit(FeedLoad{limit - 1024 + 1, 1024}, kTimestampBaseNs);
    ASSERT_FALSE(admitted);
    EXPECT_EQ(admitted.error().code(), ErrorCode::ipc_queue_full);
}

TEST(ConsumerDecodeAdmission, QueueAtLimitRejectsNonemptyAu) {
    const std::uint64_t limit = DecodeSessionState::kMaxFeedBacklogBytes;
    DecodeSessionState state;
    const auto one_byte = state.admit(FeedLoad{limit, 1}, kTimestampBaseNs);
    ASSERT_FALSE(one_byte);
    EXPECT_EQ(one_byte.error().code(), ErrorCode::ipc_queue_full);
    const auto regular = state.admit(FeedLoad{limit, 4096}, kTimestampBaseNs);
    ASSERT_FALSE(regular);
    EXPECT_EQ(regular.error().code(), ErrorCode::ipc_queue_full);
}

TEST(ConsumerDecodeAdmission, HugeQueueLevelCannotOverflowAdmission) {
    // A saturated byte counter must never wrap the subtraction into acceptance
    // and an oversized payload must be refused before any subtraction.
    const std::uint64_t limit = DecodeSessionState::kMaxFeedBacklogBytes;
    const std::uint64_t huge_levels[] = {DecodeSessionState::kClockTimeNone, limit + 1, limit};
    const std::size_t payloads[] = {std::size_t{1}, static_cast<std::size_t>(limit),
                                    static_cast<std::size_t>(limit) + 1U};
    DecodeSessionState state;
    for (const std::uint64_t queued : huge_levels) {
        for (const std::size_t payload : payloads) {
            const auto admitted = state.admit(FeedLoad{queued, payload}, kTimestampBaseNs);
            ASSERT_FALSE(admitted) << queued << "/" << payload;
            EXPECT_EQ(admitted.error().code(), ErrorCode::ipc_queue_full);
        }
    }
}

TEST(ConsumerDecodeAdmission, QueueRejectionDoesNotConsumeTimestamp) {
    const std::uint64_t limit = DecodeSessionState::kMaxFeedBacklogBytes;
    DecodeSessionState state;
    const auto rejected = state.admit(FeedLoad{limit, 1024}, kTimestampBaseNs);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::ipc_queue_full);
    EXPECT_FALSE(state.last_accepted_timestamp_ns.has_value());
    // After the backlog drains the SAME timestamp must be admitted.
    const auto retried = state.admit(FeedLoad{0, 1024}, kTimestampBaseNs);
    ASSERT_TRUE(retried) << static_cast<int>(retried.error().code());
    state.accept_timestamp(kTimestampBaseNs);
    EXPECT_EQ(state.last_accepted_timestamp_ns, kTimestampBaseNs);
}

TEST(ConsumerDecodeAdmission, DuplicateDeliveredTimestampRejected) {
    // The high-water mark is not a live-map artifact: once a timestamp was
    // accepted it stays consumed even after its correlation would have been
    // delivered and released (later timestamps accepted below model that).
    DecodeSessionState state;
    ASSERT_TRUE(state.admit(FeedLoad{0, 64}, kTimestampBaseNs));
    state.accept_timestamp(kTimestampBaseNs);
    const std::uint64_t later = kTimestampBaseNs + kFramePeriodNs;
    ASSERT_TRUE(state.admit(FeedLoad{0, 64}, later));
    state.accept_timestamp(later);
    const auto duplicate_first = state.admit(FeedLoad{0, 64}, kTimestampBaseNs);
    ASSERT_FALSE(duplicate_first);
    EXPECT_EQ(duplicate_first.error().code(), ErrorCode::invalid_argument);
    const auto duplicate_last = state.admit(FeedLoad{0, 64}, later);
    ASSERT_FALSE(duplicate_last);
    EXPECT_EQ(duplicate_last.error().code(), ErrorCode::invalid_argument);
}

TEST(ConsumerDecodeAdmission, RegressingTimestampRejected) {
    DecodeSessionState state;
    const std::uint64_t t0 = kTimestampBaseNs;
    const std::uint64_t t1 = kTimestampBaseNs + kFramePeriodNs;
    ASSERT_TRUE(state.admit(FeedLoad{0, 64}, t1));
    state.accept_timestamp(t1);
    const auto regressing = state.admit(FeedLoad{0, 64}, t0);
    ASSERT_FALSE(regressing);
    EXPECT_EQ(regressing.error().code(), ErrorCode::invalid_argument);
    const auto equal = state.admit(FeedLoad{0, 64}, t1);
    ASSERT_FALSE(equal);
    EXPECT_EQ(equal.error().code(), ErrorCode::invalid_argument);
}

TEST(ConsumerDecodeAdmission, ClockTimeNoneRejected) {
    // GST_CLOCK_TIME_NONE (UINT64_MAX) is never a valid AU timestamp, not even
    // as the first one of a session.
    DecodeSessionState state;
    const auto first = state.admit(FeedLoad{0, 64}, DecodeSessionState::kClockTimeNone);
    ASSERT_FALSE(first);
    EXPECT_EQ(first.error().code(), ErrorCode::invalid_argument);
    EXPECT_FALSE(state.last_accepted_timestamp_ns.has_value());
    ASSERT_TRUE(state.admit(FeedLoad{0, 64}, kTimestampBaseNs));
    state.accept_timestamp(kTimestampBaseNs);
    const auto later = state.admit(FeedLoad{0, 64}, DecodeSessionState::kClockTimeNone);
    ASSERT_FALSE(later);
    EXPECT_EQ(later.error().code(), ErrorCode::invalid_argument);
}

TEST(ConsumerDecodeAdmission, ZeroFirstTimestampAccepted) {
    DecodeSessionState state;
    const auto first = state.admit(FeedLoad{0, 64}, 0);
    ASSERT_TRUE(first) << static_cast<int>(first.error().code());
    state.accept_timestamp(0);
    EXPECT_EQ(state.last_accepted_timestamp_ns, 0U);
    const auto next = state.admit(FeedLoad{0, 64}, 1);
    ASSERT_TRUE(next) << static_cast<int>(next.error().code());
}

TEST(ConsumerDecodeAdmission, RejectedSubmissionDoesNotAdvanceTimestamp) {
    DecodeSessionState state;
    ASSERT_TRUE(state.admit(FeedLoad{0, 64}, kTimestampBaseNs));
    state.accept_timestamp(kTimestampBaseNs);
    const std::uint64_t next_ts = kTimestampBaseNs + kFramePeriodNs;
    const std::size_t oversized =
        static_cast<std::size_t>(DecodeSessionState::kMaxFeedBacklogBytes) + 1U;
    const auto rejected = state.admit(FeedLoad{0, oversized}, next_ts);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::ipc_queue_full);
    ASSERT_TRUE(state.last_accepted_timestamp_ns.has_value());
    EXPECT_EQ(*state.last_accepted_timestamp_ns, kTimestampBaseNs);
    const auto retry = state.admit(FeedLoad{0, 64}, next_ts);
    ASSERT_TRUE(retry) << static_cast<int>(retry.error().code());
    const auto duplicate = state.admit(FeedLoad{0, 64}, kTimestampBaseNs);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), ErrorCode::invalid_argument);
}

TEST(ConsumerDecodeAdmission, DuplicateLiveTimestampRejected) {
    // Public-API integration on the forced-software path: a valid fixture
    // frame is submitted once and its still-live timestamp resubmitted, which
    // must fail typed. The resubmission acquires no correlation mapping and
    // consumes no timestamp, so the following strictly-increasing AU is still
    // admitted.
    const DecodeReport probe = DecodeHelper::probe_backends();
    const bool software_viable =
        std::any_of(probe.backends.begin(), probe.backends.end(),
                    [](const BackendCandidate& c) { return !c.hardware && c.viable; });
    if (!software_viable) {
        GTEST_SKIP() << "no viable host software H.264 decoder on this host";
    }
    const auto stream = read_file(fixture_path());
    ASSERT_FALSE(stream.empty()) << "fixture missing: " << fixture_path();
    const auto units = parse_annexb_units(stream);
    ASSERT_GE(units.size(), 2U) << "fixture grammar drift";

    DecodeConfig config;
    config.caps = kPrimaryCaps;
    config.backend = DecodeBackend::software;
    auto helper = DecodeHelper::open(std::move(config));
    ASSERT_TRUE(helper) << static_cast<int>(helper.error().code());

    const VideoAccessUnit au{0, 0, kTimestampBaseNs, units[0].idr, units[0].data};
    const auto first = helper.value().submit(au);
    ASSERT_TRUE(first) << static_cast<int>(first.error().code());

    const auto duplicate = helper.value().submit(au);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), ErrorCode::invalid_argument);

    const VideoAccessUnit following{1, 1, kTimestampBaseNs + kFramePeriodNs, units[1].idr,
                                    units[1].data};
    const auto next = helper.value().submit(following);
    ASSERT_TRUE(next) << static_cast<int>(next.error().code());
    helper.value().close();
}

// ---------------------------------------------------------------------------
// Correlation lifecycle (PR #47 fix round): pending correlations are bounded
// in age by the fixed five-second retention timeout (an OPERATIONAL RETENTION
// POLICY, not a latency budget or a qualification claim) and by the 512-entry
// cap. Any of those bounds INVALIDATES THE ENTIRE pending set and arms an IDR
// resync (awaiting_idr): non-IDR AUs are then discarded with
// ipc_consumer_rejected ("discard this AU and resume at an IDR", NOT
// retryable queue-full) until an IDR passes the unchanged timestamp + byte
// admission. These cases drive the GStreamer-free DecodeSessionState helper
// with controlled clocks - the SAME methods DecodeHelper::submit/pump call -
// so the lifecycle is deterministic and never races the pipeline.
// ---------------------------------------------------------------------------

TEST(ConsumerDecodeCorrelation, DroppedAuExpiresAndRequiresIdr) {
    using Clock = DecodeSessionState::Clock;
    const Clock::time_point submitted_at{};
    DecodeSessionState state;

    // Two AUs admitted at the fixed clock; the first output arrives (its
    // mapping is taken), the second is dropped inside the decoder and never
    // produces an output.
    const std::uint64_t ts_dropped = kTimestampBaseNs + kFramePeriodNs;
    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, kTimestampBaseNs, true, submitted_at));
    state.accept_timestamp(kTimestampBaseNs);
    state.record_correlation(1, 1, kTimestampBaseNs, submitted_at);
    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, ts_dropped, false, submitted_at));
    state.accept_timestamp(ts_dropped);
    state.record_correlation(2, 2, ts_dropped, submitted_at);
    ASSERT_TRUE(state.take_correlation(kTimestampBaseNs).has_value());
    ASSERT_EQ(state.pending_count(), 1U);

    // Before the retention bound the dropped AU's mapping still exists and a
    // no-op expiry evaluation leaves everything in place.
    const Clock::time_point just_before =
        submitted_at + DecodeSessionState::kCorrelationRetention - std::chrono::nanoseconds{1};
    EXPECT_NE(state.find_correlation(ts_dropped), nullptr);
    state.expire_stale_correlations(just_before);
    EXPECT_EQ(state.pending_count(), 1U);
    EXPECT_FALSE(state.awaiting_idr);

    // Exactly at the bound the ENTIRE outstanding set is sacrificed and the
    // session arms an IDR resync.
    const Clock::time_point at_bound = submitted_at + DecodeSessionState::kCorrelationRetention;
    state.expire_stale_correlations(at_bound);
    EXPECT_EQ(state.pending_count(), 0U);
    EXPECT_TRUE(state.awaiting_idr);

    // While resyncing, a non-IDR AU is discarded: ipc_consumer_rejected, not a
    // retryable queue-full.
    const auto rejected =
        state.admit_submission(FeedLoad{0, 64}, ts_dropped + kFramePeriodNs, false, at_bound);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::ipc_consumer_rejected);
}

TEST(ConsumerDecodeCorrelation, PendingCapacityDoesNotBlockRecoveryIdr) {
    using Clock = DecodeSessionState::Clock;
    const Clock::time_point now{};
    DecodeSessionState state;

    // 512 tiny AUs with no outputs at a fixed clock fill the pending set.
    const std::size_t cap = DecodeSessionState::kMaxPendingCorrelations;
    for (std::size_t i = 0; i < cap; ++i) {
        const std::uint64_t frame = static_cast<std::uint64_t>(i);
        const std::uint64_t timestamp_ns = kTimestampBaseNs + frame * kFramePeriodNs;
        ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, timestamp_ns, i == 0, now)) << i;
        state.accept_timestamp(timestamp_ns);
        state.record_correlation(frame, 3 * frame + 1, timestamp_ns, now);
    }
    ASSERT_EQ(state.pending_count(), cap);

    // The next non-IDR meets a saturated set: the whole set is invalidated
    // first (submit() can never wedge on a permanently full map), then the AU
    // is discarded as a resync rejection.
    const std::uint64_t frame_next = static_cast<std::uint64_t>(cap);
    const std::uint64_t ts_next = kTimestampBaseNs + frame_next * kFramePeriodNs;
    const auto rejected = state.admit_submission(FeedLoad{0, 64}, ts_next, false, now);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_TRUE(state.awaiting_idr);
    EXPECT_EQ(state.pending_count(), 0U);

    // A valid newer IDR is not blocked by the resync: still subject to the
    // timestamp + byte admission, accepted, and leaves exactly its own single
    // pending entry with the resync cleared.
    const std::uint64_t frame_recovery = frame_next + 1;
    const std::uint64_t ts_recovery = kTimestampBaseNs + frame_recovery * kFramePeriodNs;
    const auto recovered =
        state.admit_submission(FeedLoad{0, 64}, ts_recovery, true, now);
    ASSERT_TRUE(recovered) << static_cast<int>(recovered.error().code());
    state.accept_timestamp(ts_recovery);
    state.record_correlation(frame_recovery, 3 * frame_recovery + 1, ts_recovery, now);
    EXPECT_EQ(state.pending_count(), 1U);
    EXPECT_FALSE(state.awaiting_idr);
    EXPECT_NE(state.find_correlation(ts_recovery), nullptr);
}

TEST(ConsumerDecodeCorrelation, LateOutputCannotAcquireRecoveryMetadata) {
    using Clock = DecodeSessionState::Clock;
    const Clock::time_point t0{};
    DecodeSessionState state;

    // Pre-recovery AU recorded, then the set is invalidated at the retention
    // bound and a recovery IDR admitted and recorded.
    const std::uint64_t ts_old = kTimestampBaseNs;
    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, ts_old, true, t0));
    state.accept_timestamp(ts_old);
    state.record_correlation(7, 11, ts_old, t0);
    const Clock::time_point stale = t0 + DecodeSessionState::kCorrelationRetention;
    state.expire_stale_correlations(stale);
    ASSERT_TRUE(state.awaiting_idr);

    const std::uint64_t ts_recovery = ts_old + kFramePeriodNs;
    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, ts_recovery, true, stale));
    state.accept_timestamp(ts_recovery);
    state.record_correlation(8, 12, ts_recovery, stale);

    // A late pre-recovery output finds no mapping: no metadata is handed out.
    EXPECT_EQ(state.find_correlation(ts_old), nullptr);
    EXPECT_FALSE(state.take_correlation(ts_old).has_value());

    // The recovery output acquires EXACTLY its own frame/sequence/timestamp.
    const auto meta = state.take_correlation(ts_recovery);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->frame, 8U);
    EXPECT_EQ(meta->sequence, 12U);
    EXPECT_EQ(meta->timestamp_ns, ts_recovery);
    EXPECT_EQ(state.pending_count(), 0U);
}

TEST(ConsumerDecodeCorrelation, NormalIdrPreservesOutstandingCorrelations) {
    using Clock = DecodeSessionState::Clock;
    const Clock::time_point now{};
    DecodeSessionState state;

    // IDR, P frame, then an ordinary IDR while NOT recovering.
    const std::uint64_t ts_first = kTimestampBaseNs;
    const std::uint64_t ts_middle = kTimestampBaseNs + kFramePeriodNs;
    const std::uint64_t ts_idr = kTimestampBaseNs + 2 * kFramePeriodNs;
    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, ts_first, true, now));
    state.accept_timestamp(ts_first);
    state.record_correlation(1, 1, ts_first, now);
    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, ts_middle, false, now));
    state.accept_timestamp(ts_middle);
    state.record_correlation(2, 2, ts_middle, now);
    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, ts_idr, true, now));
    state.accept_timestamp(ts_idr);
    state.record_correlation(3, 3, ts_idr, now);

    // An ordinary IDR never invalidates: the prior mappings remain deliverable
    // with their own metadata.
    EXPECT_FALSE(state.awaiting_idr);
    ASSERT_EQ(state.pending_count(), 3U);
    const auto first = state.take_correlation(ts_first);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->frame, 1U);
    EXPECT_EQ(first->sequence, 1U);
    EXPECT_EQ(first->timestamp_ns, ts_first);
    const auto middle = state.take_correlation(ts_middle);
    ASSERT_TRUE(middle.has_value());
    EXPECT_EQ(middle->frame, 2U);
    EXPECT_EQ(middle->sequence, 2U);
    EXPECT_EQ(middle->timestamp_ns, ts_middle);
}

TEST(ConsumerDecodeCorrelation, RejectedRecoveryIdrPreservesResyncState) {
    using Clock = DecodeSessionState::Clock;
    const Clock::time_point t0{};
    DecodeSessionState state;

    ASSERT_TRUE(state.admit_submission(FeedLoad{0, 64}, kTimestampBaseNs, true, t0));
    state.accept_timestamp(kTimestampBaseNs);
    state.record_correlation(1, 1, kTimestampBaseNs, t0);
    const Clock::time_point stale = t0 + DecodeSessionState::kCorrelationRetention;
    state.expire_stale_correlations(stale);
    ASSERT_TRUE(state.awaiting_idr);

    // An oversized recovery IDR is byte-rejected with the retryable
    // ipc_queue_full (timestamp contract passed; it may retry after a drain).
    const std::uint64_t ts_recovery = kTimestampBaseNs + kFramePeriodNs;
    const std::size_t oversized =
        static_cast<std::size_t>(DecodeSessionState::kMaxFeedBacklogBytes) + 1U;
    const auto rejected = state.admit_submission(FeedLoad{0, oversized}, ts_recovery, true, stale);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::ipc_queue_full);

    // The rejection preserves the resync state: awaiting_idr stays armed and
    // the timestamp high-water mark is untouched (no reset at IDR recovery).
    EXPECT_TRUE(state.awaiting_idr);
    ASSERT_TRUE(state.last_accepted_timestamp_ns.has_value());
    EXPECT_EQ(*state.last_accepted_timestamp_ns, kTimestampBaseNs);
    EXPECT_EQ(state.pending_count(), 0U);
}
