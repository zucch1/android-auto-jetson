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
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>

#include "ProjectionWidget.hpp"

#include "../../src/ipc/dbus_adapter/BusConnection.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

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

TEST(DummyDisplayContractRender, PaintCompletionRetainsCorrelationAndIgnoresRepaints) {
    // Given two queued frames; only the newest can actually be painted.
    static_cast<void>(qt_app());
    CountingInputSink input;
    aa::display::qt_adapter::ProjectionWidget widget(input, VideoProfile{2, 2, 30});
    aa::consumer::DecodedFrame frame{7, 4, 123456, 2, 2, {80, 80, 80, 80, 128, 128}};
    testing::internal::CaptureStdout();
    widget.on_frame(frame);
    frame.frame = 8;
    frame.timestamp_ns = 234567;
    widget.on_frame(frame);
    const auto before = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // When painting the queued frame twice (the second is a repaint, not progress).
    static_cast<void>(widget.grab());
    static_cast<void>(widget.grab());
    const auto after = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto output = testing::internal::GetCapturedStdout();
    std::smatch match;
    ASSERT_TRUE(std::regex_search(output, match, std::regex{
        "\"event\":\"video_presented\",\"frame\":8,\"scheduled_ns\":234567,"
        "\"completed_ns\":([0-9]+)"})) << output;
    // Then completion is stamped during paint, exactly once, with retained metadata.
    EXPECT_GE(std::stoll(match[1].str()), before);
    EXPECT_LE(std::stoll(match[1].str()), after);
    EXPECT_EQ(output.find("video_presented", output.find("video_presented") + 1),
              std::string::npos);
}

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

    // Task-27 PhoneDirectory seam (fail-closed): pairing is always rejected,
    // cancellation is a no-op and nothing ever expires.
    [[nodiscard]] aa::core::Result<void> pairing_requested(aa::core::PairingRequestId) override {
        return aa::Error{ErrorCode::invalid_argument};
    }
    [[nodiscard]] aa::core::Result<aa::core::PhoneId> pairing_confirmed(
        aa::core::PairingRequestId) override {
        return aa::Error{ErrorCode::invalid_argument};
    }
    void pairing_cancelled(aa::core::PairingRequestId) override {}
    [[nodiscard]] std::vector<aa::core::PairingRequestId> reap_expired() override { return {}; }
    [[nodiscard]] std::vector<aa::core::PairingRequestId> take_invalidated_pairings() override {
        return {};
    }
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

    // And: the failed registration UNWOUND its name acquisition - a third
    // connection can acquire the rejected client's proposed name WHILE the
    // rejected client is still alive (the old code left the name owned by the
    // failed registrant, and a queued request would even inherit it later).
    auto third = aa::ipc::dbus_adapter::BusConnection::connect_session();
    ASSERT_TRUE(third.has_value());
    const auto acquired =
        third.value().request_name(BusName{"org.custom.DummyDisplayTestB2"});
    EXPECT_TRUE(acquired.has_value())
        << "rejected registrant must not keep (or queue) its proposed name";
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

TEST(DummyDisplayContractInput, LetterboxedTouchMapsIntoVideoPixels) {
    // Given: a 1280x720 surface resized to 4:3, so the video is letterboxed
    // into a centered 640x360 area with 60 px bars above and below.
    qt_app();
    CountingInputSink sink;
    aa::display::qt_adapter::ProjectionWidget widget(sink, kCaps);
    widget.resize(640, 480);
    ASSERT_EQ(widget.video_rect(), QRect(0, 60, 640, 360));
    const auto send = [&widget](QEvent::Type type, QPointF at) {
        QMouseEvent event(type, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &event);
    };

    // When: a gesture starts in the letterbox bar, then one starts inside the
    // video and drags past its edge.
    send(QEvent::MouseButtonPress, QPointF(10.0, 20.0));
    send(QEvent::MouseMove, QPointF(30.0, 30.0));
    send(QEvent::MouseButtonRelease, QPointF(30.0, 30.0));
    send(QEvent::MouseButtonPress, QPointF(320.0, 240.0));
    send(QEvent::MouseMove, QPointF(700.0, 470.0));
    send(QEvent::MouseButtonRelease, QPointF(700.0, 470.0));

    // Then: the bar gesture is dropped entirely; the video gesture is scaled
    // into absolute video pixels and clamped to the frame.
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(std::get<TouchEvent>(sink.events[0]), (TouchEvent{640, 360, TouchAction::down}));
    EXPECT_EQ(std::get<TouchEvent>(sink.events[1]), (TouchEvent{1279, 719, TouchAction::move}));
    EXPECT_EQ(std::get<TouchEvent>(sink.events[2]), (TouchEvent{1279, 719, TouchAction::up}));
}

TEST(DummyDisplayContractInput, ViewShortcutsStayLocal) {
    // Given: a display surface with the HUD shown by default.
    qt_app();
    CountingInputSink sink;
    aa::display::qt_adapter::ProjectionWidget widget(sink, kCaps);
    ASSERT_TRUE(widget.hud_visible());

    // When: F1 (HUD) and F11 (full screen) are pressed and released.
    for (const int key : {Qt::Key_F1, Qt::Key_F11}) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(&widget, &press);
        QApplication::sendEvent(&widget, &release);
    }

    // Then: the HUD toggled and nothing reached the phone input seam.
    EXPECT_FALSE(widget.hud_visible());
    EXPECT_TRUE(sink.events.empty());
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

// ---------------------------------------------------------------------------
// Stop-recovery regressions (PR #36 fix round). The tests below drive the
// failed-stop decision table through a TEST-ONLY scripted contract receiver:
// a raw BusConnection owning the REAL contract name on the private
// dbus-run-session bus, answering the wire contract via read_message() /
// send_reply() (daemon-stamped sender/serial preserved, non-call traffic
// skipped, teardown bounded). Together with a recording DecodeSession and a
// real video socket this exercises every failure shape - no production mock
// interface exists or is added.
namespace {

namespace bus = aa::ipc::dbus_adapter;
using aa::ipc::OutboundReply;
using aa::ipc::StateSnapshot;

// One scripted answer for a contract member. `ok` carries the member's
// payload (RegisterConsumer id / GetState snapshot), `error` is a typed error
// reply, `withhold` reads the call and never answers (the client sees the
// real BusIo read timeout) and `malformed` returns a wire-valid method return
// with trailing body bytes past the contract's declared reply payload.
struct ScriptedReply final {
    enum class Kind { ok, error, withhold, malformed } kind{Kind::ok};
    std::string error_name{"org.custom.AndroidAutoReceiver1.Error.IllegalTransition"};
    std::uint64_t consumer{1};
    bool projecting{true};
};

// TEST-ONLY scripted receiver: per-member reply scripts (steps are consumed
// in order, the last step repeats) plus a call log so tests can prove what
// did and did not reach the receiver.
class ScriptedReceiver final {
public:
    using Script = std::map<std::string, std::vector<ScriptedReply>>;

    static std::unique_ptr<ScriptedReceiver> start(Script script) {
        auto receiver = std::unique_ptr<ScriptedReceiver>(new ScriptedReceiver(std::move(script)));
        auto connected = bus::BusConnection::connect_session();
        if (!connected.has_value()) {
            return nullptr;
        }
        receiver->connection_.emplace(std::move(connected.value()));
        const auto owned = receiver->connection_->request_name(
            BusName{std::string{aa::ipc::kContractBusName}});
        if (!owned.has_value()) {
            return nullptr;
        }
        receiver->worker_ = std::thread([ptr = receiver.get()] { ptr->run(); });
        return receiver;
    }

    ~ScriptedReceiver() {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (connection_.has_value()) {
            static_cast<void>(
                connection_->release_name(BusName{std::string{aa::ipc::kContractBusName}}));
            connection_.reset();
        }
    }

    ScriptedReceiver(const ScriptedReceiver&) = delete;
    ScriptedReceiver& operator=(const ScriptedReceiver&) = delete;

    [[nodiscard]] std::vector<std::string> calls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    [[nodiscard]] std::size_t count_calls(const std::string& member) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(std::count(calls_.begin(), calls_.end(), member));
    }

private:
    explicit ScriptedReceiver(Script script) : script_(std::move(script)) {}

    static ScriptedReply default_reply(const std::string& member) {
        ScriptedReply reply{};
        if (member == "RegisterConsumer") {
            reply.consumer = 1;
        }
        return reply;
    }

    ScriptedReply next_reply(const std::string& member) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = script_.find(member);
        if (found == script_.end() || found->second.empty()) {
            return default_reply(member);
        }
        auto& steps = found->second;
        const std::size_t index = std::min(next_[member], steps.size() - 1);
        ++next_[member];
        return steps[index];
    }

    void handle(const bus::ParsedMessage& message) {
        const ScriptedReply reply = next_reply(message.member);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            calls_.push_back(message.member);
        }
        switch (reply.kind) {
        case ScriptedReply::Kind::withhold:
            return;
        case ScriptedReply::Kind::malformed: {
            // Wire-VALID but contract-malformed: the signature covers the
            // trailing bytes so the daemon relays the reply, while the
            // client's strict registration parse must reject the trailing
            // content past the declared "t" payload.
            bus::MessageWriter writer;
            writer.reply_serial(message.serial).destination(message.sender).body_signature("tu");
            writer.put_u64(reply.consumer).put_u32(0x2A2A2A2A);
            static_cast<void>(connection_->send_message(bus::MessageType::method_return, writer));
            return;
        }
        case ScriptedReply::Kind::error: {
            OutboundReply out{};
            out.is_error = true;
            out.error_name = reply.error_name;
            out.body = std::string{"scripted rejection"};
            static_cast<void>(
                connection_->send_reply(message.serial, BusName{message.sender}, out));
            return;
        }
        case ScriptedReply::Kind::ok: {
            OutboundReply out{};
            if (message.member == "RegisterConsumer") {
                out.body = reply.consumer;
            } else if (message.member == "GetState") {
                out.body = StateSnapshot{
                    reply.projecting ? ProjectionState::projecting : ProjectionState::stopped,
                    aa::core::ConsumerId{reply.consumer}};
            }
            static_cast<void>(
                connection_->send_reply(message.serial, BusName{message.sender}, out));
            return;
        }
        }
    }

    void run() {
        while (running_.load()) {
            // Fast-wakeup poll: bounded teardown without touching the
            // production read timeout; read_message() still owns framing.
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
                if (message.error().code() == ErrorCode::transport_closed) {
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

    Script script_{};
    std::map<std::string, std::size_t> next_{};
    std::vector<std::string> calls_{};
    mutable std::mutex mutex_{};
    std::optional<bus::BusConnection> connection_{};
    std::atomic<bool> running_{true};
    std::thread worker_{};
};

// Recording DecodeSession: counts every seam call so "session not closed" and
// "closed exactly once" are directly assertable.
class RecordingSession final : public DecodeSession {
public:
    RecordingSession(int* opens, int* closes, int* submits, int* eofs)
        : opens_(opens), closes_(closes), submits_(submits), eofs_(eofs) {}

    [[nodiscard]] aa::core::Result<void> submit(const aa::ipc::VideoAccessUnit&) override {
        ++*submits_;
        return {};
    }
    [[nodiscard]] aa::core::Result<void> end_of_stream() override {
        ++*eofs_;
        return {};
    }
    [[nodiscard]] aa::core::Result<std::size_t> pump(Milliseconds) override {
        return std::size_t{0};
    }
    void close() noexcept override { ++*closes_; }

private:
    int* opens_;
    int* closes_;
    int* submits_;
    int* eofs_;
};

// Real video socket endpoint: start_projection() must connect the production
// video path, so the tests serve a real SOCK_SEQPACKET channel (one consumer).
class VideoEndpoint final {
public:
    VideoEndpoint() {
        runtime_ = private_runtime_dir();
        socket_ = aa::ipc::video_socket_path_in(runtime_);
        auto server = aa::ipc::VideoServer::listen(
            socket_, aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
        if (!server.has_value()) {
            return;
        }
        server_.emplace(std::move(server.value()));
        holder_ = std::thread([this] {
            while (running_.load()) {
                auto producer = server_->accept_consumer(Milliseconds{200});
                if (!producer.has_value()) {
                    continue;
                }
                while (running_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                }
                producer.value().close();
            }
        });
    }

    ~VideoEndpoint() {
        running_.store(false);
        if (holder_.joinable()) {
            holder_.join();
        }
        if (server_.has_value()) {
            server_->close();
        }
        if (!runtime_.empty()) {
            ::rmdir(runtime_.c_str());
        }
    }

    VideoEndpoint(const VideoEndpoint&) = delete;
    VideoEndpoint& operator=(const VideoEndpoint&) = delete;

    [[nodiscard]] bool ready() const { return server_.has_value(); }
    [[nodiscard]] const aa::ipc::VideoSocketPath& socket() const { return socket_; }

private:
    std::string runtime_{};
    aa::ipc::VideoSocketPath socket_{};
    std::optional<aa::ipc::VideoServer> server_{};
    std::atomic<bool> running_{true};
    std::thread holder_{};
};

// Common bring-up: scripted receiver + real client + real video socket +
// recording decode factory, attached but not started.
class StopFixture final {
public:
    static std::unique_ptr<StopFixture> bring_up(const ScriptedReceiver::Script& script,
                                                 const std::string& suffix,
                                                 bool fail_decode = false) {
        auto fixture = std::unique_ptr<StopFixture>(new StopFixture());
        fixture->fail_decode_ = fail_decode;
        fixture->receiver = ScriptedReceiver::start(script);
        if (!fixture->receiver) {
            return nullptr;
        }
        auto client = ControlBusClient::connect();
        if (!client.has_value()) {
            return nullptr;
        }
        fixture->client = std::make_unique<ControlBusClient>(std::move(client.value()));
        fixture->video = std::make_unique<VideoEndpoint>();
        if (!fixture->video->ready()) {
            return nullptr;
        }
        DisplayController::Deps deps{*fixture->client, fixture->recording_factory(),
                                     fixture->frames};
        auto controller = DisplayController::attach(std::move(deps), test_config(suffix));
        if (!controller.has_value()) {
            return nullptr;
        }
        fixture->controller.emplace(std::move(controller.value()));
        return fixture;
    }

    [[nodiscard]] DisplayController& display() { return controller.value(); }
    [[nodiscard]] const aa::ipc::VideoSocketPath& socket() const { return video->socket(); }

    std::unique_ptr<ScriptedReceiver> receiver{};
    std::unique_ptr<ControlBusClient> client{};
    std::unique_ptr<VideoEndpoint> video{};
    RecordingFrameTarget frames{};
    int opens{0};
    int closes{0};
    int submits{0};
    int eofs{0};

private:
    StopFixture() = default;

    [[nodiscard]] DecodeSessionFactory recording_factory() {
        if (fail_decode_) {
            return [opens = &opens](const aa::consumer::DecodeConfig&)
                       -> aa::core::Result<std::unique_ptr<DecodeSession>> {
                ++*opens;
                return aa::Error{ErrorCode::channel_decode_unavailable};
            };
        }
        return [opens = &opens, closes = &closes, submits = &submits, eofs = &eofs](
                   const aa::consumer::DecodeConfig&)
                   -> aa::core::Result<std::unique_ptr<DecodeSession>> {
            ++*opens;
            return std::unique_ptr<DecodeSession>(
                std::make_unique<RecordingSession>(opens, closes, submits, eofs));
        };
    }

    bool fail_decode_{false};
    std::optional<DisplayController> controller{};
};

} // namespace

TEST(DummyDisplayStopRecovery, RejectedStopRetainsMediaThenRetryRecovers) {
    // (a) the receiver rejects StopProjection and still reports projecting:
    // the ORIGINAL error is returned, the media is retained in stop_failed,
    // a new start is rejected before any remote StartProjection, and a
    // successful retry closes the media exactly once and lands idle.
    ScriptedReceiver::Script script{};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::error},
                                ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    script["GetState"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok, .consumer = 1,
                                        .projecting = true}};
    auto fixture = StopFixture::bring_up(script, "StopRejected");
    ASSERT_NE(fixture, nullptr);
    ASSERT_EQ(fixture->display().state(), DisplayState::idle);
    ASSERT_TRUE(fixture->display().start_projection(fixture->socket()).has_value());
    ASSERT_EQ(fixture->display().state(), DisplayState::projecting);

    // When: the stop is rejected and the snapshot still reports projecting.
    const auto failed = fixture->display().stop_projection();

    // Then: the original typed error is reported and inactivity stays
    // unconfirmed - media retained, start prohibited, stop retry allowed.
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_EQ(fixture->display().state(), DisplayState::stop_failed);
    EXPECT_EQ(fixture->closes, 0);
    EXPECT_TRUE(fixture->display().helper_started());
    ASSERT_TRUE(fixture->display().pump(Milliseconds{0}).has_value());

    // When: a new start is requested while unconfirmed.
    const auto denied = fixture->display().start_projection(fixture->socket());

    // Then: it is rejected locally, BEFORE any remote StartProjection.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_EQ(fixture->receiver->count_calls("StartProjection"), 1U);

    // When: the stop retry succeeds.
    const auto recovered = fixture->display().stop_projection();

    // Then: media closes exactly once and the state recovers to idle.
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(fixture->display().state(), DisplayState::idle);
    EXPECT_EQ(fixture->closes, 1);
    EXPECT_EQ(fixture->receiver->count_calls("StopProjection"), 2U);
}

TEST(DummyDisplayStopRecovery, NeverReplyingStopTimesOutThenRetryRecovers) {
    // (b) the receiver never replies to StopProjection: the client sees the
    // REAL BusIo read timeout (transport_io), the same stop_failed shape as
    // (a) results, and the retry recovers.
    ScriptedReceiver::Script script{};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::withhold},
                                ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    script["GetState"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok, .consumer = 1,
                                        .projecting = true}};
    auto fixture = StopFixture::bring_up(script, "StopTimeout");
    ASSERT_NE(fixture, nullptr);
    ASSERT_TRUE(fixture->display().start_projection(fixture->socket()).has_value());

    // When: the stop is never answered.
    const auto failed = fixture->display().stop_projection();

    // Then: the read timeout is the reported original error and the state is
    // stop_failed with the media retained.
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(fixture->display().state(), DisplayState::stop_failed);
    EXPECT_EQ(fixture->closes, 0);
    ASSERT_FALSE(fixture->display().start_projection(fixture->socket()).has_value());
    EXPECT_EQ(fixture->receiver->count_calls("StartProjection"), 1U);

    // When: the retry is answered.
    const auto recovered = fixture->display().stop_projection();

    // Then: media closes exactly once and idle returns.
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(fixture->display().state(), DisplayState::idle);
    EXPECT_EQ(fixture->closes, 1);
}

TEST(DummyDisplayStopRecovery, WithheldReplyButStoppedSnapshotRecoversToIdle) {
    // (c) the stop EXECUTED but its reply was withheld: the snapshot confirms
    // stopped, so the media is closed exactly once and the state recovers to
    // idle - while the ORIGINAL transport_io is still returned (the failure
    // is reported even when reality recovered).
    ScriptedReceiver::Script script{};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::withhold}};
    script["GetState"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok, .consumer = 1,
                                        .projecting = false}};
    auto fixture = StopFixture::bring_up(script, "StopWithheld");
    ASSERT_NE(fixture, nullptr);
    ASSERT_TRUE(fixture->display().start_projection(fixture->socket()).has_value());

    // When: the stop executes but its reply never arrives.
    const auto failed = fixture->display().stop_projection();

    // Then: the original error is returned, the media is released once and
    // the confirmed reality (stopped) is the shown state.
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(fixture->display().state(), DisplayState::idle);
    EXPECT_EQ(fixture->closes, 1);
    EXPECT_EQ(fixture->display().consumer(), aa::core::ConsumerId{1});
}

TEST(DummyDisplayStopRecovery, StopAndReconciliationBothFailRetainStopFailed) {
    // (d) the stop fails AND the GetState reconciliation fails: inactivity
    // stays unconfirmed - media retained, stop_failed.
    ScriptedReceiver::Script script{};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::error}};
    script["GetState"] = {ScriptedReply{.kind = ScriptedReply::Kind::error,
                                        .error_name =
                                            "org.custom.AndroidAutoReceiver1.Error.PeerUnauthorized"}};
    auto fixture = StopFixture::bring_up(script, "StopBothFail");
    ASSERT_NE(fixture, nullptr);
    ASSERT_TRUE(fixture->display().start_projection(fixture->socket()).has_value());

    // When: both the stop and the reconciliation are rejected.
    const auto failed = fixture->display().stop_projection();

    // Then: the original stop error is returned and the unconfirmed state
    // retains the media.
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_EQ(fixture->display().state(), DisplayState::stop_failed);
    EXPECT_EQ(fixture->closes, 0);
}

TEST(DummyDisplayStopRecovery, LostRegistrationReconcilesToUnavailableAndClearsId) {
    // (e) the reconciliation snapshot no longer names this consumer (zero or
    // foreign id): the media is released, the consumer id cleared and the
    // shown state becomes unavailable.
    for (const std::uint64_t snapshot_consumer : {0ULL, 999ULL}) {
        ScriptedReceiver::Script script{};
        script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::error}};
        script["GetState"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok,
                                            .consumer = snapshot_consumer, .projecting = true}};
        auto fixture = StopFixture::bring_up(script, "StopLost" + std::to_string(snapshot_consumer));
        ASSERT_NE(fixture, nullptr);
        ASSERT_TRUE(fixture->display().start_projection(fixture->socket()).has_value());

        // When: the snapshot reports a different/zero consumer.
        const auto failed = fixture->display().stop_projection();

        // Then: lost registration wins - unavailable, cleared id, media
        // released exactly once, the ORIGINAL stop error still reported.
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::session_illegal_transition);
        EXPECT_EQ(fixture->display().state(), DisplayState::unavailable);
        EXPECT_EQ(fixture->display().consumer(), aa::core::ConsumerId{});
        EXPECT_EQ(fixture->closes, 1);
    }
}

TEST(DummyDisplayStopRecovery, StartupRollbackFailureIsNotIdleUnlessStoppedConfirmed) {
    // (f) startup media failure (decode factory) plus a FAILED rollback stop
    // whose snapshot still reports projecting: the ORIGINAL startup error is
    // returned and the state is stop_failed - never a false idle.
    ScriptedReceiver::Script script{};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::error}};
    script["GetState"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok, .consumer = 1,
                                        .projecting = true}};
    auto fixture = StopFixture::bring_up(script, "StopStartupFail", true);
    ASSERT_NE(fixture, nullptr);

    // When: projection start fails at the decode factory and the rollback
    // stop is rejected.
    const auto started = fixture->display().start_projection(fixture->socket());

    // Then: the original startup error is preserved and the state is NOT
    // idle (inactivity unconfirmed), with no session ever started.
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::channel_decode_unavailable);
    EXPECT_EQ(fixture->display().state(), DisplayState::stop_failed);
    EXPECT_FALSE(fixture->display().helper_started());
    EXPECT_EQ(fixture->opens, 1);
    EXPECT_EQ(fixture->closes, 0);
}

TEST(DummyDisplayStopRecovery, StartupRollbackWithStoppedSnapshotLandsIdle) {
    // (f, confirmed variant) the rollback stop is rejected but the snapshot
    // confirms stopped: the ORIGINAL startup error is still returned while
    // the state is allowed to land idle (stopped was confirmed).
    ScriptedReceiver::Script script{};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::error}};
    script["GetState"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok, .consumer = 1,
                                        .projecting = false}};
    auto fixture = StopFixture::bring_up(script, "StopStartupStop", true);
    ASSERT_NE(fixture, nullptr);

    // When: projection start fails and the rollback stop is rejected while
    // the snapshot reports stopped.
    const auto started = fixture->display().start_projection(fixture->socket());

    // Then: the original error is preserved and the confirmed reality (idle)
    // is the shown state.
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::channel_decode_unavailable);
    EXPECT_EQ(fixture->display().state(), DisplayState::idle);
    EXPECT_FALSE(fixture->display().helper_started());
}

namespace {

class OwnedRuntime final {
public:
    OwnedRuntime() : path_(private_runtime_dir()) {}
    ~OwnedRuntime() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }
    const std::string& path() const { return path_; }
    OwnedRuntime(const OwnedRuntime&) = delete;
    OwnedRuntime& operator=(const OwnedRuntime&) = delete;
private:
    std::string path_;
};

class FactoryLatch final {
public:
    bool hold() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        return condition_.wait_for(lock, std::chrono::seconds{5}, [this] { return released_; });
    }
    bool await_entered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds{2}, [this] { return entered_; });
    }
    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{false};
    bool released_{false};
};

class StartupGuard final {
public:
    StartupGuard(FactoryLatch& latch, std::thread& worker) : latch_(latch), worker_(worker) {}
    ~StartupGuard() {
        latch_.release();
        if (worker_.joinable()) {
            worker_.join();
        }
    }
private:
    FactoryLatch& latch_;
    std::thread& worker_;
};

void exercise_factory_barrier(bool fail_while_held) {
    // Given: a registered display consumer and a live video server
    ScriptedReceiver::Script script{};
    script["StartProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    auto receiver = ScriptedReceiver::start(script);
    ASSERT_NE(receiver, nullptr);

    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());

    OwnedRuntime runtime;
    ASSERT_FALSE(runtime.path().empty());
    const auto socket = aa::ipc::video_socket_path_in(runtime.path());
    auto server = aa::ipc::VideoServer::listen(socket, aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
    ASSERT_TRUE(server.has_value());

    FactoryLatch latch;
    int opens = 0;
    int closes = 0;
    int submits = 0;
    int eofs = 0;

    auto barrier_factory = [&](const aa::consumer::DecodeConfig&) -> aa::core::Result<std::unique_ptr<DecodeSession>> {
        ++opens;
        if (!latch.hold() || fail_while_held) {
            return aa::Error{ErrorCode::timeout};
        }
        return std::unique_ptr<DecodeSession>(std::make_unique<RecordingSession>(&opens, &closes, &submits, &eofs));
    };

    RecordingFrameTarget frames;
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), barrier_factory, frames},
        test_config("StartupBarrier"));
    ASSERT_TRUE(controller.has_value());

    // When: starting projection asynchronously with decode factory held on barrier
    std::optional<aa::core::Result<void>> starter_result;
    std::thread starter([&] { starter_result.emplace(controller.value().start_projection(socket)); });
    StartupGuard guard(latch, starter);
    ASSERT_TRUE(latch.await_entered());
    if (fail_while_held) {
        FAIL() << "injected held-factory assertion";
    }

    // Then: remote StartProjection was invoked but no video connection was accepted yet
    EXPECT_EQ(receiver->count_calls("StartProjection"), 1U);
    ::pollfd descriptor{server.value().native_handle(), POLLIN, 0};
    EXPECT_EQ(::poll(&descriptor, 1, 150), 0);
    latch.release();
    auto accepted = server.value().accept_consumer(Milliseconds{2000});
    ASSERT_TRUE(accepted.has_value());
    starter.join();
    ASSERT_TRUE(starter_result.has_value());
    ASSERT_TRUE(starter_result->has_value());
    EXPECT_EQ(controller.value().state(), DisplayState::projecting);
    EXPECT_EQ(opens, 1);
}

} // namespace

TEST(DummyDisplayStartupReadiness, FactoryRunsBeforeVideoConnectionWithBarrier) {
    exercise_factory_barrier(false);
}

TEST(DummyDisplayStartupReadiness, FatalAssertionReleasesHeldFactoryBeforeJoining) {
    const auto before = std::chrono::steady_clock::now();
    EXPECT_FATAL_FAILURE(exercise_factory_barrier(true), "injected held-factory assertion");
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds{2});
}

TEST(DummyDisplayStartupReadiness, DecoderFailureRejectsBeforeAnyAcceptedVideoConsumer) {
    // Given: a registered display consumer and a failing decode factory
    ScriptedReceiver::Script script{};
    script["StartProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    auto receiver = ScriptedReceiver::start(script);
    ASSERT_NE(receiver, nullptr);

    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());

    OwnedRuntime runtime;
    ASSERT_FALSE(runtime.path().empty());
    const auto socket = aa::ipc::video_socket_path_in(runtime.path());
    auto server = aa::ipc::VideoServer::listen(socket, aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
    ASSERT_TRUE(server.has_value());

    int opens = 0;
    auto failing_factory = [&](const aa::consumer::DecodeConfig&) -> aa::core::Result<std::unique_ptr<DecodeSession>> {
        ++opens;
        return aa::Error{ErrorCode::channel_decode_unavailable};
    };

    RecordingFrameTarget frames;
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), failing_factory, frames},
        test_config("StartupFailNoAccept"));
    ASSERT_TRUE(controller.has_value());

    // When: projection start is attempted
    const auto started = controller.value().start_projection(socket);

    // Then: start fails with decode error, rolls back remote projection, and never connects video
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::channel_decode_unavailable);
    EXPECT_EQ(controller.value().state(), DisplayState::idle);
    EXPECT_EQ(receiver->count_calls("StartProjection"), 1U);
    EXPECT_EQ(receiver->count_calls("StopProjection"), 1U);

    EXPECT_EQ(opens, 1);
    EXPECT_FALSE(controller.value().helper_started());
    ::pollfd descriptor{server.value().native_handle(), POLLIN, 0};
    EXPECT_EQ(::poll(&descriptor, 1, 150), 0);
}

TEST(DummyDisplayStartupReadiness, ConnectFailureClosesHelperExactlyOnceAndRollsBack) {
    // Given: successful decode factory but unreachable video socket
    OwnedRuntime runtime;
    ASSERT_FALSE(runtime.path().empty());
    ScriptedReceiver::Script script{};
    script["StartProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok}};
    auto receiver = ScriptedReceiver::start(script);
    ASSERT_NE(receiver, nullptr);

    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());

    int opens = 0;
    int closes = 0;
    int submits = 0;
    int eofs = 0;
    auto factory = [&](const aa::consumer::DecodeConfig&) -> aa::core::Result<std::unique_ptr<DecodeSession>> {
        ++opens;
        return std::unique_ptr<DecodeSession>(std::make_unique<RecordingSession>(&opens, &closes, &submits, &eofs));
    };

    RecordingFrameTarget frames;
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), factory, frames},
        test_config("ConnectFailCloseHelper"));
    ASSERT_TRUE(controller.has_value());

    const auto non_existent_socket = aa::ipc::video_socket_path_in(runtime.path());

    // When: start projection is called
    const auto started = controller.value().start_projection(non_existent_socket);

    // Then: connect failure is returned, helper is closed once, and projection rolled back
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::transport_unavailable);
    EXPECT_EQ(controller.value().state(), DisplayState::idle);
    EXPECT_EQ(receiver->count_calls("StartProjection"), 1U);
    EXPECT_EQ(receiver->count_calls("StopProjection"), 1U);
    EXPECT_EQ(opens, 1);
    EXPECT_EQ(closes, 1);
    EXPECT_FALSE(controller.value().helper_started());
}

TEST(DummyDisplayStartupReadiness, ConnectFailurePreservesOriginalErrorWhenRollbackRejected) {
    // Given: a different typed rollback failure and an owned, unbound video path.
    OwnedRuntime runtime;
    ASSERT_FALSE(runtime.path().empty());
    ScriptedReceiver::Script script;
    script["StopProjection"] = {ScriptedReply{.kind = ScriptedReply::Kind::error}};
    script["GetState"] = {ScriptedReply{.projecting = true}};
    auto receiver = ScriptedReceiver::start(script);
    ASSERT_NE(receiver, nullptr);
    auto client = ControlBusClient::connect();
    ASSERT_TRUE(client.has_value());
    int opens = 0, closes = 0, submits = 0, eofs = 0;
    auto factory = [&](const aa::consumer::DecodeConfig&)
        -> aa::core::Result<std::unique_ptr<DecodeSession>> {
        ++opens;
        return std::unique_ptr<DecodeSession>(
            std::make_unique<RecordingSession>(&opens, &closes, &submits, &eofs));
    };
    RecordingFrameTarget frames;
    auto controller = DisplayController::attach(
        DisplayController::Deps{client.value(), factory, frames}, test_config("ConnectRollbackReject"));
    ASSERT_TRUE(controller.has_value());
    // When: connect fails and rollback is rejected as illegal-transition.
    const auto started = controller.value().start_projection(
        aa::ipc::video_socket_path_in(runtime.path()));
    // Then: transport failure is preserved, helper closes once, inactivity is unconfirmed.
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::transport_unavailable);
    EXPECT_EQ(controller.value().state(), DisplayState::stop_failed);
    EXPECT_FALSE(controller.value().helper_started());
    EXPECT_EQ(opens, 1);
    EXPECT_EQ(closes, 1);
    EXPECT_EQ(receiver->count_calls("StopProjection"), 1U);
    EXPECT_EQ(receiver->count_calls("GetState"), 1U);
    controller.value().shutdown();
    EXPECT_EQ(closes, 1);
}

// ---------------------------------------------------------------------------
// Registration regressions (PR #36 fix round): the ControlBusClient
// registration lifecycle on the REAL bus against the test-only scripted
// receiver - failed registrations must unwind their name acquisition
// (malformed and withheld replies included), duplicate registrations must
// not disturb the existing name, and a successful unregister must release
// the name and permit re-registration. The name queueing assertions need the
// real daemon (fakes do not model daemon queues).
namespace {

std::unique_ptr<ControlBusClient> connected_client() {
    auto client = ControlBusClient::connect();
    if (!client.has_value()) {
        return nullptr;
    }
    return std::make_unique<ControlBusClient>(std::move(client.value()));
}

// A raw third-party connection used ONLY to probe real name ownership.
[[nodiscard]] bool third_party_can_own(const std::string& name) {
    auto probe = aa::ipc::dbus_adapter::BusConnection::connect_session();
    if (!probe.has_value()) {
        return false;
    }
    return probe.value().request_name(BusName{name}).has_value();
}

} // namespace

TEST(DummyDisplayRegistration, MalformedRegistrationReplyUnwindsTheName) {
    // Given: a scripted receiver whose RegisterConsumer reply carries
    // trailing bytes past the declared signature.
    ScriptedReceiver::Script script{};
    script["RegisterConsumer"] = {ScriptedReply{.kind = ScriptedReply::Kind::malformed,
                                                .consumer = 1}};
    auto receiver = ScriptedReceiver::start(script);
    ASSERT_NE(receiver, nullptr);
    auto client = connected_client();
    ASSERT_NE(client, nullptr);

    // When: the registration is answered with the malformed reply.
    const auto registered = client->register_consumer(BusName{"org.custom.DummyDisplayTestRegM"});

    // Then: the typed parse failure is reported and the just-acquired name
    // was unwound - a third party can own it while the failed client lives.
    ASSERT_FALSE(registered.has_value());
    EXPECT_EQ(registered.error().code(), ErrorCode::ipc_malformed_request);
    EXPECT_TRUE(third_party_can_own("org.custom.DummyDisplayTestRegM"));
}

TEST(DummyDisplayRegistration, WithheldRegistrationReplyUnwindsTheName) {
    // Given: a scripted receiver that reads RegisterConsumer and never
    // answers (the client meets the real BusIo read timeout).
    ScriptedReceiver::Script script{};
    script["RegisterConsumer"] = {ScriptedReply{.kind = ScriptedReply::Kind::withhold}};
    auto receiver = ScriptedReceiver::start(script);
    ASSERT_NE(receiver, nullptr);
    auto client = connected_client();
    ASSERT_NE(client, nullptr);

    // When: the registration reply never arrives.
    const auto registered = client->register_consumer(BusName{"org.custom.DummyDisplayTestRegW"});

    // Then: transport_io is reported and the name was unwound all the same.
    ASSERT_FALSE(registered.has_value());
    EXPECT_EQ(registered.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(receiver->count_calls("RegisterConsumer"), 1U);
    EXPECT_TRUE(third_party_can_own("org.custom.DummyDisplayTestRegW"));
}

TEST(DummyDisplayRegistration, DuplicateRegistrationKeepsExistingNameAndUnregisterReleasesIt) {
    // Given: one successful registration holding its name.
    ScriptedReceiver::Script script{};
    script["UnregisterConsumer"] = {ScriptedReply{.kind = ScriptedReply::Kind::ok},
                                    ScriptedReply{.kind = ScriptedReply::Kind::error}};
    auto receiver = ScriptedReceiver::start(script);
    ASSERT_NE(receiver, nullptr);
    auto client = connected_client();
    ASSERT_NE(client, nullptr);
    const auto registered = client->register_consumer(BusName{"org.custom.DummyDisplayTestRegD"});
    ASSERT_TRUE(registered.has_value());

    // When: the SAME client attempts a second registration under a new name.
    const auto duplicate = client->register_consumer(BusName{"org.custom.DummyDisplayTestRegD2"});

    // Then: the duplicate is rejected BEFORE any remote RegisterConsumer or
    // name request, and the EXISTING name is untouched (still owned).
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_EQ(receiver->count_calls("RegisterConsumer"), 1U);
    EXPECT_FALSE(third_party_can_own("org.custom.DummyDisplayTestRegD"));

    // When: the registration is unregistered successfully.
    ASSERT_TRUE(client->unregister_consumer().has_value());

    // Then: the tracked name is released (a third party can own it) and a
    // fresh registration is allowed again.
    EXPECT_TRUE(third_party_can_own("org.custom.DummyDisplayTestRegD"));
    const auto again = client->register_consumer(BusName{"org.custom.DummyDisplayTestRegD"});
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(receiver->count_calls("RegisterConsumer"), 2U);

    // When: a later unregister fails.
    const auto failed = client->unregister_consumer();

    // Then: the tracked name is RETAINED - a duplicate registration stays
    // rejected and the name stays owned.
    ASSERT_FALSE(failed.has_value());
    EXPECT_FALSE(third_party_can_own("org.custom.DummyDisplayTestRegD"));
    const auto still = client->register_consumer(BusName{"org.custom.DummyDisplayTestRegD3"});
    ASSERT_FALSE(still.has_value());
    EXPECT_EQ(still.error().code(), ErrorCode::session_illegal_transition);
}
