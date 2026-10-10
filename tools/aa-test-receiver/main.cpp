// SPDX-License-Identifier: GPL-3.0-or-later
// TEST-ONLY runnable receiver-side integration (task 24 F4): a small binary
// that owns the task-21 D-Bus name/object through ControlBusService and
// dispatches continuously from its own event loop at the documented idle
// refresh interval (ControlBusService::kIdleOwnerRefreshInterval), serves the
// task-22 replay fixture over the video channel and receives display input
// from the task-24 input endpoint. It exists so the SHIPPED aa-dummy-display
// can be exercised end-to-end outside the gtest harness - this is test-only
// tooling, not the production receiver (that is later task scope).
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/PairingGate.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/ipc/VideoSocket.hpp>

#include "annexb_units.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <signal.h>
#include <unistd.h>

namespace {

using aa::core::Milliseconds;

std::atomic<bool> running{true};
std::mutex log_mutex;

void log_line(const std::string& json) {
    const std::lock_guard<std::mutex> lock(log_mutex);
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void on_signal(int) { running.store(false); }

class SequenceIds final : public aa::ipc::PairingIdSource {
public:
    [[nodiscard]] aa::core::PairingRequestId next() override {
        return aa::core::PairingRequestId{next_++};
    }

private:
    std::uint64_t next_{1000};
};

class RejectPhones final : public aa::ipc::PhoneDirectory {
public:
    [[nodiscard]] aa::core::Result<void> forget(aa::core::PhoneId) override {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
};

std::string event_input(const aa::ipc::InputEvent& event) {
    if (const auto* key = std::get_if<aa::ipc::KeyEvent>(&event)) {
        return std::string("{\"event\":\"input\",\"kind\":\"key\",\"keycode\":") +
               std::to_string(key->keycode) + ",\"pressed\":" + (key->pressed ? "true" : "false") +
               "}";
    }
    const auto& touch = std::get<aa::ipc::TouchEvent>(event);
    const char* action = touch.action == aa::ipc::TouchAction::down
                             ? "down"
                             : (touch.action == aa::ipc::TouchAction::move ? "move" : "up");
    return std::string("{\"event\":\"input\",\"kind\":\"touch\",\"x\":") + std::to_string(touch.x) +
           ",\"y\":" + std::to_string(touch.y) + ",\"action\":\"" + action + "\"}";
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

// Serves the replay fixture to each accepted display-side video consumer.
void serve_video(aa::ipc::VideoServer& server, const std::vector<aatestreceiver::AnnexBUnit>& units) {
    while (running.load()) {
        auto producer = server.accept_consumer(Milliseconds{200});
        if (!producer) {
            continue;
        }
        log_line("{\"event\":\"video_accepted\"}");
        std::size_t sent = 0;
        for (std::size_t index = 0; index < units.size() && running.load(); ++index) {
            const auto status = producer.value().send_access_unit(aa::ipc::VideoSendRequest{
                static_cast<std::uint64_t>(index),
                5'000'000'000ULL + static_cast<std::uint64_t>(index) * 33'333'333ULL,
                units[index].idr, units[index].data});
            if (!status.has_value()) {
                break;
            }
            ++sent;
        }
        producer.value().close();
        log_line("{\"event\":\"video_sent\",\"units\":" + std::to_string(sent) + "}");
    }
}

// Receives normalized input from the display-side sender: the typed endpoint
// todo 26's input service will consume.
void serve_input(aa::ipc::InputServer& server) {
    while (running.load()) {
        auto endpoint = server.accept_sender(Milliseconds{200});
        if (!endpoint) {
            continue;
        }
        log_line("{\"event\":\"input_accepted\"}");
        while (running.load()) {
            auto event = endpoint.value().receive(Milliseconds{200});
            if (!event) {
                if (event.error().code() == aa::ErrorCode::timeout) {
                    continue;
                }
                break;
            }
            log_line(event_input(event.value()));
        }
        endpoint.value().close();
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string video_socket;
    std::string input_socket;
    std::string fixture;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--video-socket") == 0 && index + 1 < argc) {
            video_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--input-socket") == 0 && index + 1 < argc) {
            input_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--fixture") == 0 && index + 1 < argc) {
            fixture = argv[++index];
        } else {
            std::fputs("usage: aa-test-receiver --video-socket P --input-socket P --fixture P\n",
                       stderr);
            return 2;
        }
    }
    if (video_socket.empty() || input_socket.empty() || fixture.empty()) {
        std::fputs("usage: aa-test-receiver --video-socket P --input-socket P --fixture P\n", stderr);
        return 2;
    }

    const auto stream = read_file(fixture);
    const auto units = aatestreceiver::parse_annexb_units(stream);
    if (units.empty()) {
        std::fputs("{\"event\":\"err\",\"detail\":\"fixture\"}\n", stdout);
        return 1;
    }

    SequenceIds ids;
    RejectPhones phones;
    auto service =
        aa::ipc::ControlBusService::start(aa::ipc::ControlBusService::Config{aa::ipc::current_uid()},
                                          aa::ipc::ControlBusService::Deps{ids, phones});
    if (!service.has_value()) {
        std::fputs("{\"event\":\"err\",\"detail\":\"dbus\"}\n", stdout);
        return 1;
    }

    auto video = aa::ipc::VideoServer::listen(
        aa::ipc::VideoSocketPath{video_socket},
        aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
    auto input = aa::ipc::InputServer::listen(aa::ipc::InputSocketPath{input_socket},
                                              aa::ipc::InputServerOptions{aa::ipc::current_uid()});
    if (!video.has_value() || !input.has_value()) {
        std::fputs("{\"event\":\"err\",\"detail\":\"socket\"}\n", stdout);
        return 1;
    }

    log_line("{\"event\":\"ready\",\"units\":" + std::to_string(units.size()) + "}");

    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    std::thread video_thread(serve_video, std::ref(video.value()), std::cref(units));
    std::thread input_thread(serve_input, std::ref(input.value()));

    std::optional<aa::ipc::StateSnapshot> last_state{};
    std::uint64_t dispatched_total = 0;
    while (running.load()) {
        // Continuous dispatch event loop at the documented idle refresh
        // interval: cycle() reconciles the consumer owner on each expired
        // idle window exactly as the ControlBusService contract documents.
        auto step = service.value().cycle(aa::ipc::ControlBusService::kIdleOwnerRefreshInterval);
        if (!step.has_value()) {
            break;
        }
        dispatched_total += step.value();
        const auto snapshot = service.value().service().state();
        if (!last_state.has_value() || !(snapshot == *last_state)) {
            last_state = snapshot;
            log_line(std::string("{\"event\":\"state\",\"projection\":\"") +
                     (snapshot.projection == aa::ipc::ProjectionState::projecting ? "projecting"
                                                                                  : "stopped") +
                     "\",\"consumer\":" + std::to_string(snapshot.consumer.value) + "}");
        }
    }

    running.store(false);
    video_thread.join();
    input_thread.join();
    log_line("{\"event\":\"stopping\",\"dispatched\":" + std::to_string(dispatched_total) +
             ",\"idle_refreshes\":" + std::to_string(service.value().idle_refreshes()) +
             ",\"unauthorized_peers\":" + std::to_string(input.value().unauthorized_peers()) + "}");
    input.value().close();
    video.value().close();
    service.value().close();
    return 0;
}
