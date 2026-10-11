// SPDX-License-Identifier: GPL-3.0-or-later
// Task 24 dummy display contract suite: the Qt 6 dummy display as an
// authorized test consumer over the REAL task-21 D-Bus contract (live service
// export with REAL declared signals and idle owner cleanup), replay-fixture
// render through the task-22 video channel and task-23 decode helper,
// task-26-shaped input normalization at the display seam, the TEST-ONLY
// autostart gate, and the receiver-absent failure state (nothing started).
//
// Determinism: the replay path feeds the checked-in AUD-delimited Baseline
// fixture (90 access units, 1280x720@30) with fixed monotonic timestamps;
// assertions are frame counts, dimensions, metadata carry-through and state
// transitions - never screenshot pixels beyond a smoke render check.
#include <aa/display/Display.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>

#include "ProjectionWidget.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace aa::display;
using aa::core::Milliseconds;
using aa::ErrorCode;
using aa::ipc::BusName;
using aa::ipc::ControlBusClient;
using aa::ipc::ProjectionState;
using aa::ipc::Viewport;
using aa::protocol::VideoProfile;

constexpr VideoProfile kCaps{1280, 720, 30};
constexpr std::uint64_t kTimestampBaseNs = 5'000'000'000ULL;
constexpr std::uint64_t kFramePeriodNs = 33'333'333ULL;

// The QApplication is deliberately leaked (never destroyed): Qt 6's input
// device singletons outlive it and crash in their destructors when it is a
// function-local static destroyed by the exit handlers (the standard Qt/gtest
// teardown-order workaround).
QApplication* qt_app() {
    static int argc = 1;
    static char name[] = "aa_dummy_display_test";
    static char* argv[] = {name, nullptr};
    static QApplication* app = new QApplication(argc, argv);
    return app;
}

std::string fixture_path() { return std::string{AA_DISPLAY_FIXTURE}; }

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
// (the same copy the consumer_decode suite feeds), so the replay stream here
// matches the established fixture grammar.
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

class CountingInputSink final : public InputSink {
public:
    void on_input(const InputEvent& event) override { events.push_back(event); }
    std::vector<InputEvent> events{};
};

struct FrameRecord final {
    std::uint64_t frame{};
    std::uint64_t sequence{};
    std::uint64_t timestamp_ns{};
    std::uint16_t width{};
    std::uint16_t height{};
};

class RecordingFrameTarget final : public FrameTarget {
public:
    void on_frame(const aa::consumer::DecodedFrame& frame) override {
        records.push_back(
            {frame.frame, frame.sequence, frame.timestamp_ns, frame.width, frame.height});
    }
    std::vector<FrameRecord> records{};
};

class TeeFrameTarget final : public FrameTarget {
public:
    TeeFrameTarget(FrameTarget& first, FrameTarget& second) : first_(first), second_(second) {}
    void on_frame(const aa::consumer::DecodedFrame& frame) override {
        first_.on_frame(frame);
        second_.on_frame(frame);
    }

private:
    FrameTarget& first_;
    FrameTarget& second_;
};

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
        return aa::Error{ErrorCode::invalid_argument};
    }

    [[nodiscard]] aa::core::Result<void> pairing_requested(aa::core::PairingRequestId) override {
        return aa::Error{ErrorCode::invalid_argument};
    }

    [[nodiscard]] aa::core::Result<aa::core::PhoneId> pairing_confirmed(
        aa::core::PairingRequestId) override {
        return aa::Error{ErrorCode::invalid_argument};
    }

    void pairing_cancelled(aa::core::PairingRequestId) override {}

    [[nodiscard]] std::vector<aa::core::PairingRequestId> reap_expired() override { return {}; }
};

struct ServiceHarness final {
    SequenceIds ids{};
    RejectPhones phones{};
    std::optional<aa::ipc::ControlBusService> service{};
    std::thread dispatch{};
    std::atomic<bool> running{true};

    static std::unique_ptr<ServiceHarness> start() {
        auto harness = std::make_unique<ServiceHarness>();
        auto started = aa::ipc::ControlBusService::start(
            aa::ipc::ControlBusService::Config{aa::ipc::current_uid()},
            aa::ipc::ControlBusService::Deps{harness->ids, harness->phones});
        if (!started.has_value()) {
            return nullptr;
        }
        harness->service.emplace(std::move(started.value()));
        harness->dispatch = std::thread([ptr = harness.get()] {
            while (ptr->running.load()) {
                auto step = ptr->service->cycle(Milliseconds{50});
                if (!step.has_value() && step.error().code() == ErrorCode::transport_closed) {
                    break;
                }
            }
        });
        return harness;
    }

    ~ServiceHarness() {
        running.store(false);
        if (dispatch.joinable()) {
            dispatch.join();
        }
        if (service.has_value()) {
            service->close();
        }
    }
};

DecodeSessionFactory counting_factory(int* opens, DecodeSessionFactory inner) {
    return [opens, inner = std::move(inner)](const aa::consumer::DecodeConfig& config) {
        ++*opens;
        return inner(config);
    };
}

DecodeSessionFactory rejecting_factory() {
    return [](const aa::consumer::DecodeConfig&) -> aa::core::Result<std::unique_ptr<DecodeSession>> {
        return aa::Error{ErrorCode::channel_decode_unavailable};
    };
}

DisplayConfig test_config(const std::string& consumer_suffix) {
    DisplayConfig config{};
    config.consumer_name = BusName{"org.custom.DummyDisplayTest" + consumer_suffix};
    config.viewport = Viewport{1280, 720, 160};
    config.caps = kCaps;
    return config;
}

std::vector<AnnexBUnit> fixture_units() {
    return parse_annexb_units(read_file(fixture_path()));
}

std::string private_runtime_dir() {
    char pattern[] = "/tmp/aa-dummy-display-XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    return dir == nullptr ? std::string{} : std::string{dir};
}

// Joins a worker thread at scope exit so assertion failures cannot skip the
// join and hit std::terminate.
class ThreadGuard final {
public:
    explicit ThreadGuard(std::thread& worker) : worker_(worker) {}
    ~ThreadGuard() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;

private:
    std::thread& worker_;
};

} // namespace

TEST(DummyDisplayContractControl, RegisterViewportStartStopOverRealContract) {
    // Given: a running live service export and the display's contract client.
    auto service = ServiceHarness::start();
    ASSERT_NE(service, nullptr);
    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());
    const auto owner = client.value().receiver_owner();
    ASSERT_TRUE(owner.has_value());
    ASSERT_TRUE(owner.value().has_value());
    qt_app();
    RecordingFrameTarget frames{};
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), rejecting_factory(), frames},
        test_config("A"));
    ASSERT_TRUE(controller.has_value());
    EXPECT_EQ(controller.value().state(), DisplayState::idle);
    EXPECT_EQ(controller.value().frames_presented(), 0U);

    // When: projection is requested and stopped through the real contract.
    ASSERT_TRUE(client.value().start_projection().has_value());
    const auto projecting = client.value().get_state();
    ASSERT_TRUE(projecting.has_value());
    ASSERT_TRUE(client.value().stop_projection().has_value());
    const auto stopped = client.value().get_state();
    ASSERT_TRUE(stopped.has_value());
    bool saw_projecting_signal = false;
    bool saw_stopped_signal = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto signal = client.value().wait_signal(Milliseconds{250});
        if (!signal.has_value()) {
            break;
        }
        if (signal.value().member == "StateChanged" &&
            signal.value().projection == ProjectionState::projecting) {
            saw_projecting_signal = true;
        }
        if (signal.value().member == "StateChanged" &&
            signal.value().projection == ProjectionState::stopped) {
            saw_stopped_signal = true;
        }
    }

    // Then: GetState tracks the transitions and the declared events arrive as
    // REAL D-Bus signals.
    EXPECT_EQ(projecting.value().projection, ProjectionState::projecting);
    EXPECT_EQ(stopped.value().projection, ProjectionState::stopped);
    EXPECT_TRUE(saw_projecting_signal);
    EXPECT_TRUE(saw_stopped_signal);
    EXPECT_EQ(controller.value().consumer(), projecting.value().consumer);
    EXPECT_EQ(controller.value().state(), DisplayState::idle);
}

TEST(DummyDisplayContractControl, SecondConsumerRejectedAndViewportRoundTrips) {
    // Given: one registered display consumer.
    auto service = ServiceHarness::start();
    ASSERT_NE(service, nullptr);
    auto first = ControlBusClient::connect();
    ASSERT_TRUE(first.has_value());
    RecordingFrameTarget frames{};
    auto controller = DisplayController::attach(
        DisplayController::Deps{first.value(), rejecting_factory(), frames},
        test_config("B"));
    ASSERT_TRUE(controller.has_value());

    // When: a second same-UID consumer tries to register.
    auto second = ControlBusClient::connect();
    ASSERT_TRUE(second.has_value());
    const auto denied = second.value().register_consumer(BusName{"org.custom.DummyDisplayTestB2"});

    // Then: the single-consumer rule rejects it typed, and read status stays
    // open to every caller.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_TRUE(second.value().ping().has_value());
}

TEST(DummyDisplayContractRender, ReplayFixtureRendersThroughHelperAndChannel) {
    // Given: a live service export, the display consumer and a replay stream
    // served over the task-22 video channel from the checked fixture.
    auto service = ServiceHarness::start();
    ASSERT_NE(service, nullptr);
    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());
    qt_app();
    CountingInputSink sink;
    aa::display::qt_adapter::ProjectionWidget widget(sink, kCaps);
    RecordingFrameTarget recorder{};
    TeeFrameTarget tee(widget, recorder);
    int helper_opens = 0;
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), counting_factory(&helper_opens,
                                                                 helper_decode_sessions()),
                                tee},
        test_config("Render"));
    ASSERT_TRUE(controller.has_value());

    const auto units = fixture_units();
    ASSERT_EQ(units.size(), 90U);
    const auto runtime = private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto socket = aa::ipc::video_socket_path_in(runtime);
    auto server = aa::ipc::VideoServer::listen(socket, aa::ipc::VideoServerOptions{{},
                                                                                   aa::ipc::current_uid()});
    ASSERT_TRUE(server.has_value());
    std::atomic<bool> feeder_ok{true};
    std::thread feeder([&] {
        auto producer = server.value().accept_consumer(Milliseconds{10000});
        if (!producer.has_value()) {
            feeder_ok.store(false);
            return;
        }
        for (std::size_t index = 0; index < units.size(); ++index) {
            auto sent = producer.value().send_access_unit(aa::ipc::VideoSendRequest{
                static_cast<std::uint64_t>(index),
                kTimestampBaseNs + static_cast<std::uint64_t>(index) * kFramePeriodNs,
                units[index].idr, units[index].data});
            if (!sent.has_value()) {
                feeder_ok.store(false);
                return;
            }
        }
        producer.value().close();
    });
    ThreadGuard feeder_guard(feeder);

    // When: the display requests projection and pumps the stream.
    const auto started = controller.value().start_projection(socket);
    ASSERT_TRUE(started.has_value()) << "start_projection failed";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while (controller.value().frames_presented() < 90 &&
           std::chrono::steady_clock::now() < deadline) {
        auto pumped = controller.value().pump(Milliseconds{50});
        ASSERT_TRUE(pumped.has_value());
    }
    static_cast<void>(controller.value().pump(Milliseconds{250}));

    // Then: the todo-23 helper decoded the todo-22 stream; every frame kept
    // the channel metadata and the correct dimensions.
    EXPECT_EQ(controller.value().frames_presented(), 90U);
    EXPECT_TRUE(feeder_ok.load());
    EXPECT_EQ(helper_opens, 1);
    ASSERT_EQ(recorder.records.size(), 90U);
    for (std::size_t index = 0; index < recorder.records.size(); ++index) {
        EXPECT_EQ(recorder.records[index].width, 1280U);
        EXPECT_EQ(recorder.records[index].height, 720U);
        EXPECT_EQ(recorder.records[index].frame, static_cast<std::uint64_t>(index));
        EXPECT_EQ(recorder.records[index].timestamp_ns,
                  kTimestampBaseNs + static_cast<std::uint64_t>(index) * kFramePeriodNs);
        if (index > 0) {
            EXPECT_LT(recorder.records[index - 1].timestamp_ns,
                      recorder.records[index].timestamp_ns);
        }
    }
    const auto render = widget.grab();
    EXPECT_FALSE(render.isNull());
    EXPECT_EQ(render.width(), 1280);
    EXPECT_EQ(render.height(), 720);
    EXPECT_GE(widget.frames_painted(), 1U);
}

TEST(DummyDisplayContractInput, KeyboardAndMouseNormalizeToSeamEvents) {
    // Given: a registered display consumer with its Qt surface active.
    auto service = ServiceHarness::start();
    ASSERT_NE(service, nullptr);
    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());
    qt_app();
    CountingInputSink sink;
    aa::display::qt_adapter::ProjectionWidget widget(sink, kCaps);
    widget.set_status(DisplayState::projecting);
    RecordingFrameTarget frames{};
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), rejecting_factory(), frames},
        test_config("Input"));
    ASSERT_TRUE(controller.has_value());

    // When: keyboard/D-pad and mouse input arrive at the Qt surface.
    QKeyEvent up_press(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QKeyEvent up_release(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    QKeyEvent key_a(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QMouseEvent down_press(QEvent::MouseButtonPress, QPointF(10.5, 20.5), QPointF(10.5, 20.5),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent drag(QEvent::MouseMove, QPointF(40.0, 50.0), QPointF(40.0, 50.0), Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, QPointF(40.0, 50.0), QPointF(40.0, 50.0),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &up_press);
    QApplication::sendEvent(&widget, &up_release);
    QApplication::sendEvent(&widget, &key_a);
    QApplication::sendEvent(&widget, &down_press);
    QApplication::sendEvent(&widget, &drag);
    QApplication::sendEvent(&widget, &up);

    // Then: the events traverse the todo-26 input seam normalized (Android
    // keycodes for keyboard/D-pad; absolute viewport pixels for mouse-to-touch).
    ASSERT_EQ(sink.events.size(), 6U);
    const auto* first_key = std::get_if<KeyEvent>(&sink.events[0]);
    ASSERT_NE(first_key, nullptr);
    EXPECT_EQ(*first_key, (KeyEvent{19, true}));
    const auto* second_key = std::get_if<KeyEvent>(&sink.events[1]);
    ASSERT_NE(second_key, nullptr);
    EXPECT_EQ(*second_key, (KeyEvent{19, false}));
    const auto* letter = std::get_if<KeyEvent>(&sink.events[2]);
    ASSERT_NE(letter, nullptr);
    EXPECT_EQ(*letter, (KeyEvent{29, true}));
    const auto* touch_down = std::get_if<TouchEvent>(&sink.events[3]);
    ASSERT_NE(touch_down, nullptr);
    EXPECT_EQ(*touch_down, (TouchEvent{10, 20, TouchAction::down}));
    const auto* touch_move = std::get_if<TouchEvent>(&sink.events[4]);
    ASSERT_NE(touch_move, nullptr);
    EXPECT_EQ(*touch_move, (TouchEvent{40, 50, TouchAction::move}));
    const auto* touch_up = std::get_if<TouchEvent>(&sink.events[5]);
    ASSERT_NE(touch_up, nullptr);
    EXPECT_EQ(*touch_up, (TouchEvent{40, 50, TouchAction::up}));
    EXPECT_EQ(widget.status_text(), QStringLiteral("projecting"));
}

TEST(DummyDisplayContractAbsent, ReceiverAbsentShowsUnavailableWithoutStartingHelper) {
    // Given: a bus with NO receiver owning the contract name.
    qt_app();
    CountingInputSink sink;
    aa::display::qt_adapter::ProjectionWidget widget(sink, kCaps);
    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());
    const auto owner = client.value().receiver_owner();
    ASSERT_TRUE(owner.has_value());
    ASSERT_FALSE(owner.value().has_value());
    int helper_opens = 0;

    // When: the display attaches anyway.
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), counting_factory(&helper_opens,
                                                                 rejecting_factory()),
                                widget},
        test_config("Absent"));
    ASSERT_TRUE(controller.has_value());
    const auto started = controller.value().start_projection(
        aa::ipc::VideoSocketPath{"/tmp/aa-dummy-display-absent.sock"});
    widget.set_status(controller.value().state());

    // Then: the unavailable state is shown and NOTHING was started: no decode
    // helper, no video channel, and (the display has no helper/NetworkManager
    // code path at all) no AP.
    EXPECT_EQ(controller.value().state(), DisplayState::unavailable);
    EXPECT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_EQ(helper_opens, 0);
    EXPECT_FALSE(controller.value().helper_started());
    EXPECT_EQ(controller.value().frames_presented(), 0U);
    EXPECT_EQ(widget.status_text(), QStringLiteral("unavailable"));
}

TEST(DummyDisplayContractIdle, IdleOwnerCleanupTearsDownAbandonedConsumer) {
    // Given: a projecting consumer whose connection then dies abruptly, and
    // an observer that only listens (so no lookup-on-use call can reconcile).
    auto service = ServiceHarness::start();
    ASSERT_NE(service, nullptr);
    {
        auto consumer = ControlBusClient::connect();
        ASSERT_TRUE(consumer.has_value());
        const auto registered =
            consumer.value().register_consumer(BusName{"org.custom.DummyDisplayTestIdle"});
        ASSERT_TRUE(registered.has_value());
        ASSERT_TRUE(consumer.value().start_projection().has_value());
    }
    auto observer = ControlBusClient::connect();
    ASSERT_TRUE(observer.has_value());

    // When: the runtime idles (cycle windows expire with no traffic and the
    // runtime calls refresh_consumer_owner() per its documented contract).
    bool owner_lost = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    while (std::chrono::steady_clock::now() < deadline && !owner_lost) {
        auto signal = observer.value().wait_signal(Milliseconds{100});
        if (signal.has_value() && signal.value().member == "DiagnosticEvent" &&
            signal.value().event == "consumer_owner_lost") {
            owner_lost = true;
        }
    }
    const auto cleared = observer.value().get_state();

    // Then: the teardown happened via the idle refresh (nothing else was
    // calling the service), exactly as the documented contract states, and a
    // fresh consumer can register.
    EXPECT_TRUE(owner_lost);
    ASSERT_TRUE(cleared.has_value());
    EXPECT_EQ(cleared.value().consumer, aa::core::ConsumerId{});
    EXPECT_GT(service->service->idle_refreshes(), 0U);
    auto successor = ControlBusClient::connect();
    ASSERT_TRUE(successor.has_value());
    const auto again =
        successor.value().register_consumer(BusName{"org.custom.DummyDisplayTestIdle2"});
    EXPECT_TRUE(again.has_value());
}

TEST(DummyDisplayContractAutostart, TestOnlyGateRunsAndRefusesWithoutMarker) {
    // Given: the TEST-ONLY autostart gate (never a production autostart).
    EXPECT_FALSE(is_test_only(TestOnlyAutostart{}));
    EXPECT_TRUE(is_test_only(TestOnlyAutostart{TestOnlyAutostart::kMarker, {}}));
    qt_app();
    CountingInputSink sink;
    aa::display::qt_adapter::ProjectionWidget widget(sink, kCaps);
    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());
    RecordingFrameTarget frames{};

    // When: the gate is presented without its exact test-only marker.
    auto refused = attach_test_only_autostart(
        TestOnlyAutostart{std::string_view{"production"}, {}},
        DisplayController::Deps{client.value(), rejecting_factory(), frames},
        test_config("AutostartNo"));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::invalid_argument);

    // When: the gate is presented correctly against a live receiver.
    auto service = ServiceHarness::start();
    ASSERT_NE(service, nullptr);
    const auto runtime = private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto socket = aa::ipc::video_socket_path_in(runtime);
    auto server = aa::ipc::VideoServer::listen(
        socket, aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
    ASSERT_TRUE(server.has_value());
    std::atomic<bool> release_holder{false};
    std::thread holder([&] {
        auto producer = server.value().accept_consumer(Milliseconds{10000});
        if (!producer.has_value()) {
            return;
        }
        while (!release_holder.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        producer.value().close();
    });
    ThreadGuard holder_guard(holder);
    int helper_opens = 0;
    auto started = attach_test_only_autostart(
        TestOnlyAutostart{TestOnlyAutostart::kMarker, socket},
        DisplayController::Deps{client.value(), counting_factory(&helper_opens,
                                                                 helper_decode_sessions()),
                                widget},
        test_config("AutostartYes"));

    // Then: the display comes up end-to-end (registered, projecting, helper
    // started) - and only through this explicitly test-only gate.
    ASSERT_TRUE(started.has_value()) << "test-only autostart failed";
    EXPECT_EQ(started.value().state(), DisplayState::projecting);
    EXPECT_TRUE(started.value().helper_started());
    EXPECT_EQ(helper_opens, 1);
    started.value().shutdown();
    release_holder.store(true);
}

namespace {

class RecordingControl final : public ProjectionControl {
public:
    void on_start_projection() override { ++starts; }
    void on_stop_projection() override { ++stops; }

    int starts{0};
    int stops{0};
};

} // namespace

TEST(DummyDisplayContractControl, ShortcutKeysDriveStartStopWithoutAutostart) {
    // Given: a live service export and a display attached WITHOUT the
    // test-only autostart gate, with the widget's control seam wired.
    auto service = ServiceHarness::start();
    ASSERT_NE(service, nullptr);
    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());
    qt_app();
    CountingInputSink sink;
    aa::display::qt_adapter::ProjectionWidget widget(sink, kCaps);
    RecordingFrameTarget frames{};
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), rejecting_factory(), frames},
        test_config("Shortcut"));
    ASSERT_TRUE(controller.has_value());
    RecordingControl control{};
    widget.set_projection_control(&control);

    // When: the F5/F6 shortcut keys hit the widget's real key path.
    QKeyEvent start_press(QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier);
    QKeyEvent start_release(QEvent::KeyRelease, Qt::Key_F5, Qt::NoModifier);
    QApplication::sendEvent(&widget, &start_press);
    QApplication::sendEvent(&widget, &start_release);
    QKeyEvent stop_press(QEvent::KeyPress, Qt::Key_F6, Qt::NoModifier);
    QKeyEvent stop_release(QEvent::KeyRelease, Qt::Key_F6, Qt::NoModifier);
    QApplication::sendEvent(&widget, &stop_press);
    QApplication::sendEvent(&widget, &stop_release);

    // Then: each shortcut invoked its control exactly once and no input
    // leaked to the phone seam from the control keys themselves.
    EXPECT_EQ(control.starts, 1);
    EXPECT_EQ(control.stops, 1);
    EXPECT_EQ(sink.events.size(), 0U);
    EXPECT_EQ(controller.value().state(), DisplayState::idle);
}

TEST(DummyDisplayContractInput, WidgetEventsTraverseInputSocketToEndpointNormalized) {
    // Given: a receiver-side input endpoint (the todo-26 seam) listening on
    // the real input socket and the widget's sink wired to the socket sender.
    const auto runtime = private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = aa::ipc::input_socket_path_in(runtime);
    auto server = aa::ipc::InputServer::listen(path, aa::ipc::InputServerOptions{aa::ipc::current_uid()});
    ASSERT_TRUE(server);
    aa::ipc::InputSender sender;
    sender.attach(path, aa::ipc::current_uid());

    qt_app();
    aa::display::qt_adapter::ProjectionWidget widget(sender, kCaps);
    QKeyEvent up_press(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QKeyEvent up_release(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    QKeyEvent key_a(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QMouseEvent down_press(QEvent::MouseButtonPress, QPointF(10.5, 20.5), QPointF(10.5, 20.5),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent drag(QEvent::MouseMove, QPointF(40.0, 50.0), QPointF(40.0, 50.0), Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, QPointF(40.0, 50.0), QPointF(40.0, 50.0),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    // When: keyboard/D-pad and mouse input hit the Qt surface.
    QApplication::sendEvent(&widget, &up_press);
    QApplication::sendEvent(&widget, &up_release);
    QApplication::sendEvent(&widget, &key_a);
    QApplication::sendEvent(&widget, &down_press);
    QApplication::sendEvent(&widget, &drag);
    QApplication::sendEvent(&widget, &up);
    ASSERT_EQ(sender.sent(), 6U);

    auto endpoint = server.value().accept_sender(Milliseconds{2000});
    ASSERT_TRUE(endpoint);

    // Then: the normalized events arrived at the receiver-side endpoint over
    // the real socket - the transport todo 26 consumes.
    const auto expect = [&endpoint](const InputEvent& wanted) {
        auto got = endpoint.value().receive(Milliseconds{2000});
        ASSERT_TRUE(got) << "endpoint missed a normalized event";
        EXPECT_EQ(got.value(), wanted);
    };
    expect(InputEvent{KeyEvent{19, true}});
    expect(InputEvent{KeyEvent{19, false}});
    expect(InputEvent{KeyEvent{29, true}});
    expect(InputEvent{TouchEvent{10, 20, TouchAction::down}});
    expect(InputEvent{TouchEvent{40, 50, TouchAction::move}});
    expect(InputEvent{TouchEvent{40, 50, TouchAction::up}});
    EXPECT_EQ(sender.dropped(), 0U);
    server.value().close();
    ::rmdir(runtime.c_str());
}
