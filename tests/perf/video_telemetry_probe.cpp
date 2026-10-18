// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wired-receiver/VideoOutlet.hpp"

#include "../video_socket/pair.hpp"
#include "../../tools/aa-test-receiver/annexb_units.hpp"
#include <aa/consumer/Decode.hpp>

#include <fstream>
#include <iterator>
#include <cstdio>

namespace {
void log_line(const std::string& line) {
    std::puts(line.c_str());
    std::fflush(stdout);
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> stream{std::istreambuf_iterator<char>{input}, {}};
    const auto units = aatestreceiver::parse_annexb_units(stream);
    if (units.size() != 90) {
        return 3;
    }
    auto pair = videotest::make_pair({});
    if (!pair) {
        return 4;
    }
    pair.value().consumer.set_observer([](const aa::ipc::VideoEndpoint& event) {
        log_line(aa::ipc::video_endpoint_json(event));
    });
    aa::consumer::DecodeConfig config{};
    config.caps = {1280, 720, 30};
    config.backend = aa::consumer::DecodeBackend::software;
    config.on_completion = [](const aa::consumer::DecodeCompletion& event) {
        log_line(aa::consumer::decode_completion_json(event));
    };
    auto decoder = aa::consumer::DecodeHelper::open(std::move(config));
    if (!decoder) {
        return 5;
    }
    aa::wired_receiver::VideoOutlet outlet{log_line};
    aa::protocol::MessageView message{};
    if (!outlet.on_message(message)) {
        return 6;
    }
    outlet.install(std::move(pair.value().producer));
    const std::array<std::byte, 1> non_idr{std::byte{1}};
    message.payload = non_idr;
    if (!outlet.on_message(message)) {
        return 7;
    }
    for (const auto& unit : units) {
        message.header.timestamp = aa::core::Nanoseconds{
            static_cast<std::int64_t>(aa::ipc::video_monotonic_ns())};
        message.payload = std::as_bytes(std::span{unit.data});
        if (!outlet.on_message(message)) {
            return 8;
        }
        for (;;) {
            const auto status = pair.value().consumer.receive(aa::core::Milliseconds{2000});
            if (!status) {
                return 9;
            }
            if (status.value() == aa::ipc::ReceiveStatus::complete) {
                break;
            }
        }
        if (!decoder.value().submit(pair.value().consumer.access_unit())) {
            return 10;
        }
    }
    if (!decoder.value().end_of_stream()) {
        return 11;
    }
    for (int attempt = 0; attempt < 200 && decoder.value().report().frames_delivered < 90; ++attempt) {
        if (!decoder.value().pump(aa::core::Milliseconds{50})) {
            return 12;
        }
    }
    decoder.value().close();
    outlet.close();
    return decoder.value().report().frames_delivered == 90 ? 0 : 13;
}
