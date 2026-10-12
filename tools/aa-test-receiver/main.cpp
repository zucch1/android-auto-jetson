// SPDX-License-Identifier: GPL-3.0-or-later
// TEST-ONLY runnable receiver-side integration (task 24 F4): a small binary
// that owns the task-21 D-Bus name/object through ControlBusService and
// dispatches continuously from its own event loop at the documented idle
// refresh interval (ControlBusService::kIdleOwnerRefreshInterval), serves the
// task-22 replay fixture over the video channel and receives display input
// from the task-24 input endpoint. It exists so the SHIPPED aa-dummy-display
// can be exercised end-to-end outside the gtest harness - this is test-only
// tooling, not the production receiver (that is later task scope).
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/PairingGate.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/ipc/VideoSocket.hpp>

#include "annexb_units.hpp"

#include "../../src/ipc/dbus_adapter/BusConnection.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <poll.h>
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

    // Task-27 PhoneDirectory seam (fail-closed): pairing is always rejected,
    // cancellation is a no-op and nothing ever expires. Declared against the
    // task-27 signatures so these become the required overrides the moment
    // that seam lands in this tree; until then the interface is forget-only.
    [[nodiscard]] aa::core::Result<void> pairing_requested(aa::core::PairingRequestId) {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    [[nodiscard]] aa::core::Result<aa::core::PhoneId> pairing_confirmed(
        aa::core::PairingRequestId) {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    void pairing_cancelled(aa::core::PairingRequestId) {}
    [[nodiscard]] std::vector<aa::core::PairingRequestId> reap_expired() { return {}; }
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

// TEST-ONLY scripted contract receiver (executable gate, PR #36 fix round):
// the `reject-first-stop` mode of this binary - a raw BusConnection owning the
// REAL contract name on the private dbus-run-session bus, answering the wire
// contract via read_message()/send_reply() exactly like the
// DummyDisplayStopRecovery controller tests do. ONE scripted scenario: the
// FIRST StopProjection is REJECTED with a typed error while GetState keeps
// reporting the SAME consumer still projecting; the stop retry is ACCEPTED
// and GetState then reports stopped. Every contract call is logged so the
// executable test can prove exactly what did (and did not) reach the
// receiver - no production mock seam exists or is added.
class ScriptedContract final {
public:
    static std::unique_ptr<ScriptedContract> start() {
        auto scripted = std::unique_ptr<ScriptedContract>(new ScriptedContract());
        auto connected = aa::ipc::dbus_adapter::BusConnection::connect_session();
        if (!connected.has_value()) {
            return nullptr;
        }
        scripted->connection_.emplace(std::move(connected.value()));
        const auto owned = scripted->connection_->request_name(
            aa::ipc::BusName{std::string{aa::ipc::kContractBusName}});
        if (!owned.has_value()) {
            return nullptr;
        }
        scripted->worker_ = std::thread([ptr = scripted.get()] { ptr->run(); });
        return scripted;
    }

    ~ScriptedContract() {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (connection_.has_value()) {
            static_cast<void>(connection_->release_name(
                aa::ipc::BusName{std::string{aa::ipc::kContractBusName}}));
            connection_.reset();
        }
    }

    ScriptedContract(const ScriptedContract&) = delete;
    ScriptedContract& operator=(const ScriptedContract&) = delete;

    [[nodiscard]] std::size_t start_calls() const { return starts_.load(); }
    [[nodiscard]] std::size_t stop_calls() const { return stops_.load(); }

private:
    ScriptedContract() = default;

    void handle(const aa::ipc::dbus_adapter::ParsedMessage& message) {
        namespace bus = aa::ipc::dbus_adapter;
        aa::ipc::OutboundReply out{};
        const std::string member = message.member;
        if (member == "RegisterConsumer") {
            out.body = std::uint64_t{1};
            log_line("{\"event\":\"call\",\"member\":\"RegisterConsumer\"}");
        } else if (member == "GetState") {
            // The stateful script: the same consumer (1) reports "projecting"
            // while the rejected stop leaves inactivity unconfirmed, and
            // "stopped" once the retry is accepted.
            const bool projecting = projecting_.load();
            out.body = aa::ipc::StateSnapshot{
                projecting ? aa::ipc::ProjectionState::projecting
                           : aa::ipc::ProjectionState::stopped,
                aa::core::ConsumerId{1}};
            log_line(std::string("{\"event\":\"call\",\"member\":\"GetState\",\"projection\":\"") +
                     (projecting ? "projecting" : "stopped") + "\",\"consumer\":1}");
        } else if (member == "StartProjection") {
            ++starts_;
            projecting_.store(true);
            log_line("{\"event\":\"call\",\"member\":\"StartProjection\"}");
            log_line("{\"event\":\"state\",\"projection\":\"projecting\",\"consumer\":1}");
        } else if (member == "StopProjection") {
            ++stops_;
            if (stops_.load() == 1) {
                // REJECT the first stop with the typed error reply (the
                // DummyDisplayStopRecovery (a) shape).
                out.is_error = true;
                out.error_name = "org.custom.AndroidAutoReceiver1.Error.IllegalTransition";
                out.body = std::string{"scripted stop rejection"};
                log_line("{\"event\":\"call\",\"member\":\"StopProjection\",\"reply\":\"error\"}");
                static_cast<void>(
                    connection_->send_reply(message.serial, aa::ipc::BusName{message.sender}, out));
                return;
            }
            // ACCEPT the stop retry: GetState reports stopped from here.
            projecting_.store(false);
            log_line("{\"event\":\"call\",\"member\":\"StopProjection\",\"reply\":\"ok\"}");
            log_line("{\"event\":\"state\",\"projection\":\"stopped\",\"consumer\":1}");
        } else {
            log_line("{\"event\":\"call\",\"member\":" + quote(member) + "}");
        }
        static_cast<void>(
            connection_->send_reply(message.serial, aa::ipc::BusName{message.sender}, out));
    }

    void run() {
        namespace bus = aa::ipc::dbus_adapter;
        while (running_.load()) {
            // Fast-wakeup poll (the DummyDisplayStopRecovery pattern): bounded
            // teardown without touching the production read timeout.
            ::pollfd descriptor{connection_->native_handle(), POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (!running_.load()) {
                break;
            }
            if (ready <= 0) {
                continue;
            }
            auto message = connection_->read_message();
            if (!message.has_value()) {
                if (message.error().code() == aa::ErrorCode::transport_closed) {
                    break;
                }
                continue;
            }
            if (message.value().type != bus::MessageType::method_call) {
                continue;
            }
            handle(message.value());
        }
    }

    static std::string quote(const std::string& value) {
        std::string out = "\"";
        out.append(value);
        out += "\"";
        return out;
    }

    std::optional<aa::ipc::dbus_adapter::BusConnection> connection_{};
    std::atomic<bool> running_{true};
    std::atomic<bool> projecting_{false};
    std::atomic<std::size_t> starts_{0};
    std::atomic<std::size_t> stops_{0};
    std::thread worker_{};
};

} // namespace

int main(int argc, char** argv) {
    static const char* const kUsage =
        "usage: aa-test-receiver --video-socket P --input-socket P --fixture P"
        " [--mode reject-first-stop]\n";
    std::string video_socket;
    std::string input_socket;
    std::string fixture;
    bool scripted_mode = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--video-socket") == 0 && index + 1 < argc) {
            video_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--input-socket") == 0 && index + 1 < argc) {
            input_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--fixture") == 0 && index + 1 < argc) {
            fixture = argv[++index];
        } else if (std::strcmp(argv[index], "--mode") == 0 && index + 1 < argc) {
            // TEST-ONLY scripted-receiver MODE (executable gate, PR #36 fix
            // round): reject-first-stop replaces the real ControlBusService
            // with the scripted contract receiver below.
            if (std::strcmp(argv[++index], "reject-first-stop") != 0) {
                std::fputs(kUsage, stderr);
                return 2;
            }
            scripted_mode = true;
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (video_socket.empty() || input_socket.empty() || fixture.empty()) {
        std::fputs(kUsage, stderr);
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
    std::unique_ptr<ScriptedContract> scripted{};
    std::optional<aa::ipc::ControlBusService> service{};
    if (scripted_mode) {
        scripted = ScriptedContract::start();
        if (scripted == nullptr) {
            std::fputs("{\"event\":\"err\",\"detail\":\"dbus\"}\n", stdout);
            return 1;
        }
    } else {
        auto started = aa::ipc::ControlBusService::start(
            aa::ipc::ControlBusService::Config{aa::ipc::current_uid()},
            aa::ipc::ControlBusService::Deps{ids, phones});
        if (!started.has_value()) {
            std::fputs("{\"event\":\"err\",\"detail\":\"dbus\"}\n", stdout);
            return 1;
        }
        service.emplace(std::move(started.value()));
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

    log_line(scripted_mode ? "{\"event\":\"ready\",\"units\":" + std::to_string(units.size()) +
                                 ",\"mode\":\"reject-first-stop\"}"
                           : "{\"event\":\"ready\",\"units\":" + std::to_string(units.size()) + "}");

    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    std::thread video_thread(serve_video, std::ref(video.value()), std::cref(units));
    std::thread input_thread(serve_input, std::ref(input.value()));

    std::uint64_t dispatched_total = 0;
    if (scripted_mode) {
        // The scripted contract answers from its own worker thread; the main
        // loop only waits for the shutdown signal.
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    } else {
        std::optional<aa::ipc::StateSnapshot> last_state{};
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
                         (snapshot.projection == aa::ipc::ProjectionState::projecting
                              ? "projecting"
                              : "stopped") +
                         "\",\"consumer\":" + std::to_string(snapshot.consumer.value) + "}");
            }
        }
    }

    running.store(false);
    video_thread.join();
    input_thread.join();
    if (scripted_mode) {
        log_line("{\"event\":\"stopping\",\"mode\":\"reject-first-stop\",\"starts\":" +
                 std::to_string(scripted->start_calls()) +
                 ",\"stops\":" + std::to_string(scripted->stop_calls()) + "}");
    } else {
        log_line("{\"event\":\"stopping\",\"dispatched\":" + std::to_string(dispatched_total) +
                 ",\"idle_refreshes\":" + std::to_string(service.value().idle_refreshes()) +
                 ",\"unauthorized_peers\":" + std::to_string(input.value().unauthorized_peers()) +
                 "}");
    }
    input.value().close();
    video.value().close();
    if (scripted_mode) {
        scripted.reset();
    } else {
        service.value().close();
    }
    return 0;
}
