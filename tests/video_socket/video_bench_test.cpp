// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct LossReport final {
    bool observed{false};
    std::uint64_t resyncs{};
    std::uint64_t recovered_frame{};
    bool recovered_idr{false};
};

struct PacedReport final {
    std::uint64_t frames_expected{};
    std::uint64_t frames_delivered{};
    std::uint64_t bytes_delivered{};
    std::uint64_t fragmented_idrs{};
    std::uint64_t corrupt{};
    std::uint64_t drops{};
    std::uint64_t backpressure{};
    double throughput_mbps{};
    double p95_ms{};
    double duration_s{};
    NegotiatedLimits limits{};
};

PacedReport paced{};
LossReport loss{};

const char* bench_report_path() {
    const char* from_env = std::getenv("AA_VIDEO_BENCH_REPORT");
    return (from_env != nullptr && from_env[0] != '\0') ? from_env : "video_bench_report.json";
}

void write_bench_report() {
    std::FILE* out = std::fopen(bench_report_path(), "w");
    if (out == nullptr) {
        return;
    }
    std::fprintf(out,
                 "{\n"
                 "  \"schema\": \"aa-video-bench-report-1\",\n"
                 "  \"qualification\": \"production AF_UNIX video channel (task 22), not the "
                 "task-10 spike run\",\n"
                 "  \"paced_stream\": {\n"
                 "    \"profile\": \"1280x720@30\",\n"
                 "    \"frames_expected\": %llu,\n"
                 "    \"frames_delivered\": %llu,\n"
                 "    \"bytes_delivered\": %llu,\n"
                 "    \"fragmented_idrs\": %llu,\n"
                 "    \"corrupt\": %llu,\n"
                 "    \"drops\": %llu,\n"
                 "    \"backpressure\": %llu,\n"
                 "    \"duration_s\": %.6f,\n"
                 "    \"throughput_mbps\": %.3f,\n"
                 "    \"p95_latency_ms\": %.3f\n"
                 "  },\n"
                 "  \"buffer_bounds\": {\n"
                 "    \"max_datagram_payload_bytes\": %llu,\n"
                 "    \"max_access_unit_bytes\": %llu,\n"
                 "    \"socket_budget_bytes\": %llu\n"
                 "  },\n"
                 "  \"induced_loss\": {\n"
                 "    \"observed\": %s,\n"
                 "    \"resyncs_after_recovery\": %llu,\n"
                 "    \"recovered_frame\": %llu,\n"
                 "    \"recovered_idr\": %s\n"
                 "  }\n"
                 "}\n",
                 static_cast<unsigned long long>(paced.frames_expected),
                 static_cast<unsigned long long>(paced.frames_delivered),
                 static_cast<unsigned long long>(paced.bytes_delivered),
                 static_cast<unsigned long long>(paced.fragmented_idrs),
                 static_cast<unsigned long long>(paced.corrupt),
                 static_cast<unsigned long long>(paced.drops),
                 static_cast<unsigned long long>(paced.backpressure), paced.duration_s,
                 paced.throughput_mbps, paced.p95_ms,
                 static_cast<unsigned long long>(paced.limits.max_datagram_payload),
                 static_cast<unsigned long long>(paced.limits.max_access_unit_bytes),
                 static_cast<unsigned long long>(paced.limits.socket_budget_bytes),
                 loss.observed ? "true" : "false",
                 static_cast<unsigned long long>(loss.resyncs),
                 static_cast<unsigned long long>(loss.recovered_frame),
                 loss.recovered_idr ? "true" : "false");
    std::fclose(out);
}
} // namespace

TEST(VideoBench, PacedStreamMeetsTask10Budgets) {
    paced = PacedReport{};
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& producer = pair.value().producer;
    auto& consumer = pair.value().consumer;

    constexpr std::uint64_t kFrames = 300;
    constexpr std::size_t kIdrBytes = 96UL * 1024;
    constexpr std::size_t kPBytes = 48UL * 1024;
    std::array<std::uint64_t, 10001> histogram{};
    std::atomic<bool> consumer_error{false};
    paced.frames_expected = kFrames;

    std::thread consumer_thread([&] {
        for (std::uint64_t received = 0; received < kFrames;) {
            const auto step = consumer.receive(aa::core::Milliseconds{5000});
            if (!step) {
                consumer_error.store(true);
                return;
            }
            if (step.value() != ReceiveStatus::complete) {
                continue;
            }
            const auto now = steady_ns();
            const auto& unit = consumer.access_unit();
            if (unit.timestamp_ns > now) {
                ++paced.corrupt;
                continue;
            }
            const auto latency = now - unit.timestamp_ns;
            ++histogram[std::min<std::uint64_t>(latency / 100000, histogram.size() - 1)];
            ++paced.frames_delivered;
            paced.bytes_delivered += unit.payload.size();
            if (unit.idr && unit.payload.size() > kMaxDatagramPayload) {
                ++paced.fragmented_idrs;
            }
            ++received;
        }
    });

    std::vector<std::uint8_t> idr(kIdrBytes, 0xa5);
    std::vector<std::uint8_t> p(kPBytes, 0x5a);
    const auto start = steady_ns();
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(static_cast<std::int64_t>(start + i * 1000000000 / 30))));
        const bool is_idr = i % 30 == 0;
        const auto outcome = producer.send_access_unit({i, steady_ns(), is_idr,
                                                        is_idr ? std::span<const std::uint8_t>{idr}
                                                               : std::span<const std::uint8_t>{p}});
        ASSERT_TRUE(outcome);
    }
    consumer_thread.join();

    paced.duration_s = static_cast<double>(steady_ns() - start) / 1e9;
    paced.throughput_mbps =
        static_cast<double>(paced.bytes_delivered) * 8 / paced.duration_s / 1e6;
    std::uint64_t cumulative = 0;
    std::size_t percentile = 0;
    const auto rank = (paced.frames_delivered * 95 + 99) / 100;
    for (; percentile + 1 < histogram.size(); ++percentile) {
        cumulative += histogram[percentile];
        if (cumulative >= rank) {
            break;
        }
    }
    paced.p95_ms = static_cast<double>(percentile + 1) / 10;
    paced.drops = producer.dropped();
    paced.backpressure = producer.backpressure_events();
    paced.limits = producer.limits();
    write_bench_report();

    ASSERT_FALSE(consumer_error.load());
    EXPECT_EQ(paced.frames_delivered, kFrames);
    EXPECT_EQ(paced.corrupt, 0U);
    EXPECT_EQ(paced.drops, 0U);
    EXPECT_GE(paced.fragmented_idrs, 9U);
    EXPECT_GE(paced.throughput_mbps, 10.0);
    EXPECT_LE(paced.p95_ms, 33.0);
}

TEST(VideoBench, InducedLossResyncsAtNextIdr) {
    loss = LossReport{};
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& producer = pair.value().producer;
    auto& consumer = pair.value().consumer;

    ASSERT_TRUE(producer.send_access_unit({1, 1, true, std::vector<std::uint8_t>(64, 0x01)}));
    for (int i = 0; i < 4; ++i) {
        const auto step = consumer.receive(aa::core::Milliseconds{1000});
        if (step && step.value() == ReceiveStatus::complete) {
            break;
        }
    }
    EXPECT_EQ(consumer.resyncs(), 1U);

    const auto producer_fd = producer.native_handle();
    std::vector<std::uint8_t> lost(kFrameHeaderBytes + 4, 0x77);
    encode_frame_header(lost, VideoFrameHeader{2, 100000, 2, 0, 1, 4, 4, false});
    (void)::send(producer_fd, lost.data(), lost.size(), MSG_NOSIGNAL);
    const auto suppressed = consumer.receive(aa::core::Milliseconds{1000});
    if (suppressed) {
        EXPECT_NE(suppressed.value(), ReceiveStatus::complete);
    }
    ASSERT_TRUE(producer.send_access_unit({3, 3, true, std::vector<std::uint8_t>(64, 0x02)}));
    for (int i = 0; i < 8; ++i) {
        const auto step = consumer.receive(aa::core::Milliseconds{1000});
        if (step && step.value() == ReceiveStatus::complete) {
            loss.observed = true;
            loss.resyncs = consumer.resyncs();
            loss.recovered_frame = consumer.access_unit().frame;
            loss.recovered_idr = consumer.access_unit().idr;
            write_bench_report();
            EXPECT_TRUE(consumer.access_unit().idr);
            EXPECT_EQ(consumer.access_unit().frame, 3U);
            EXPECT_EQ(consumer.resyncs(), 2U);
            return;
        }
    }
    loss.observed = true;
    write_bench_report();
    FAIL() << "no clean IDR resync after induced loss";
}
