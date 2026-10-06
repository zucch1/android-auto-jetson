// SPDX-License-Identifier: GPL-3.0-or-later
#include "benchmark.hpp"
#include "transport.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>

namespace aa::ipc {
FailureReport failure_scenario() {
    SocketPair sockets;
    Sender sender(sockets.producer());
    auto receiver = std::make_unique<Reassembler>();
    Packet packet{};
    std::array<std::uint8_t, 48UL * 1024> bytes{};
    std::uint64_t id = 1;
    if (sender.send_frame(bytes, id++, true, monotonic_ns()) != Send::sent) return {};
    bool eof = false;
    for (unsigned i = 0; i < 3; ++i) receive(sockets.consumer(), packet, *receiver, eof);
    if (receiver->waiting_idr() || receiver->resyncs != 1) return {};
    // With no consumer reads, kernel backpressure must occur within a finite burst.
    for (unsigned i = 0; i < 100; ++i) sender.send_frame(bytes, id++, false, monotonic_ns());
    if (!sender.backpressure || !sender.drops) return {};
    pollfd ready{sockets.consumer(), POLLIN, 0};
    while (::poll(&ready, 1, 0) > 0) receive(sockets.consumer(), packet, *receiver, eof);
    // A missing datagram is followed by a P frame, which must not escape to the consumer.
    Header h{id++, 100000, monotonic_ns(), 0, 1, 4, 4, false};
    encode(packet, h);
    if (::send(sockets.producer(), packet.data(), header_size + 4, MSG_NOSIGNAL) != static_cast<ssize_t>(header_size + 4)) return {};
    if (receive(sockets.consumer(), packet, *receiver, eof) != Receive::waiting) return {};
    const auto before = receiver->resyncs;
    if (sender.send_frame(bytes, id++, true, monotonic_ns()) != Send::sent) return {};
    Receive result{};
    for (unsigned i = 0; i < 3; ++i) result = receive(sockets.consumer(), packet, *receiver, eof);
    const bool recovered = result == Receive::complete && receiver->header().idr &&
        receiver->frame().size() == bytes.size() && receiver->resyncs == before + 1;
    h = {id++, 200000, monotonic_ns(), 0, 1, static_cast<std::uint32_t>(frame_limit + 1), 4, true};
    encode(packet, h);
    ::send(sockets.producer(), packet.data(), header_size + 4, MSG_NOSIGNAL);
    const bool oversize = receive(sockets.consumer(), packet, *receiver, eof) == Receive::rejected;
    packet[4] = 99;
    ::send(sockets.producer(), packet.data(), header_size + 4, MSG_NOSIGNAL);
    const bool malformed = receive(sockets.consumer(), packet, *receiver, eof) == Receive::rejected;
    std::array<std::uint8_t, header_size + payload_limit + 1> huge{};
    ::send(sockets.producer(), huge.data(), huge.size(), MSG_NOSIGNAL);
    const bool truncated = receive(sockets.consumer(), packet, *receiver, eof) == Receive::rejected;
    return {sender.drops, sender.backpressure, receiver->resyncs, receiver->rejected,
            receiver->suppressed, recovered && oversize && malformed && truncated && receiver->waiting_idr()};
}
int benchmark(unsigned duration) {
    SocketPair sockets;
    Sender sender(sockets.producer());
    auto receiver = std::make_unique<Reassembler>();
    std::array<std::uint64_t, 10001> histogram{};
    std::uint64_t delivered = 0, delivered_bytes = 0, fragmented_idrs = 0, corrupt = 0;
    bool consumer_error = false;
    const auto start = monotonic_ns();
    std::thread consumer([&] {
        try {
            Packet packet{}; bool eof = false;
            while (!eof) {
                const auto result = receive(sockets.consumer(), packet, *receiver, eof);
                if (result != Receive::complete) continue;
                const auto now = monotonic_ns();
                const auto& h = receiver->header();
                if (h.timestamp_ns > now) { ++corrupt; continue; }
                const auto latency = now - h.timestamp_ns;
                ++histogram[std::min<std::uint64_t>(latency / 100000, histogram.size() - 1)];
                ++delivered; delivered_bytes += receiver->frame().size();
                if (h.idr && h.count > 1) ++fragmented_idrs;
                if (!std::all_of(receiver->frame().begin(), receiver->frame().end(),
                    [&](std::uint8_t byte) { return byte == static_cast<std::uint8_t>(h.frame); })) ++corrupt;
            }
        } catch (...) { consumer_error = true; ::shutdown(sockets.consumer(), SHUT_RDWR); }
    });
    std::array<std::uint8_t, 96UL * 1024> bytes{};
    const auto frames = static_cast<std::uint64_t>(duration) * 30;
    bool send_error = false;
    for (std::uint64_t i = 0; i < frames; ++i) {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(start + i * 1000000000 / 30)));
        const bool idr = i % 30 == 0;
        std::fill(bytes.begin(), bytes.end(), static_cast<std::uint8_t>(i));
        const auto result = sender.send_frame({bytes.data(), idr ? bytes.size() : bytes.size() / 2}, i, idr, monotonic_ns());
        if (result == Send::error || result == Send::invalid) { send_error = true; break; }
        if (i % 900 == 0) std::cerr << "heartbeat frame=" << i << " elapsed_s=" << (monotonic_ns() - start) / 1000000000 << '\n';
    }
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(start + static_cast<std::uint64_t>(duration) * 1000000000)));
    ::shutdown(sockets.producer(), SHUT_WR);
    consumer.join();
    const double wall = static_cast<double>(monotonic_ns() - start) / 1e9;
    const double throughput = static_cast<double>(delivered_bytes) * 8 / wall / 1e6;
    std::uint64_t cumulative = 0; std::size_t percentile = 0;
    const auto rank = (delivered * 95 + 99) / 100;
    for (; percentile < histogram.size() - 1; ++percentile) {
        cumulative += histogram[percentile]; if (cumulative >= rank) break;
    }
    // Upper bucket edge conservatively bounds nearest-rank p95; overflow fails the budget.
    const double p95 = static_cast<double>(percentile + 1) / 10;
    const auto failure = failure_scenario();
    rusage usage{}; ::getrusage(RUSAGE_SELF, &usage);
    const bool pass = !send_error && !consumer_error && delivered == frames && !sender.drops &&
        !receiver->rejected && !corrupt && fragmented_idrs == duration && throughput >= 10 && p95 <= 33 && failure.pass;
    std::cout << std::boolalpha << std::fixed << std::setprecision(6)
        << "{\"schema\":\"aa-ipc-bench/1\",\"scope\":\"host-only synthetic encoded-AU transport; no decode qualification\","
        << "\"profile\":\"1280x720@30\",\"duration_s\":" << duration << ",\"observed_duration_s\":" << wall
        << ",\"throughput_mbps\":" << throughput << ",\"p95_added_latency_ms\":" << p95
        << ",\"frames_expected\":" << frames << ",\"frames_delivered\":" << delivered
        << ",\"bytes_delivered\":" << delivered_bytes << ",\"fragmented_idrs\":" << fragmented_idrs
        << ",\"happy_drops\":" << sender.drops << ",\"corrupt_frames\":" << corrupt
        << ",\"datagram_payload_limit_bytes\":" << payload_limit << ",\"access_unit_limit_bytes\":" << frame_limit
        << ",\"userspace_queue_frames\":0,\"reassembly_slots\":1,\"socket_send_buffer_bytes\":" << sockets.send_buffer()
        << ",\"socket_send_buffer_bound_bytes\":" << 2 * socket_budget << ",\"bounded_queue\":true"
        << ",\"max_rss_kib\":" << usage.ru_maxrss << ",\"induced_loss\":{\"drops\":" << failure.drops
        << ",\"backpressure_events\":" << failure.backpressure << ",\"resyncs\":" << failure.resyncs
        << ",\"rejected\":" << failure.rejected << ",\"suppressed\":" << failure.suppressed
        << ",\"clean_idr_resync\":" << failure.pass << "},\"pass\":" << pass << "}\n";
    return pass ? 0 : 1;
}
}
