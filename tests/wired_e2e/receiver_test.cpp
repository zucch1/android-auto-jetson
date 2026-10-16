// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual harness adapters, not copies of their logging logic.
#define main wired_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include)
#undef main

#include "harness.hpp"
#include "../trust/syscall_faults.hpp"
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <gtest/gtest.h>
#include <condition_variable>
#include <unistd.h>

namespace {
std::atomic<int> runtime_connects{0};
std::atomic<int> runtime_ciphers{0};
bool runtime_recovers = false;
bool runtime_quiet = false;
std::atomic<bool> runtime_waiting{false};
std::atomic<bool> runtime_closed{false};
std::mutex runtime_owner_mutex;
std::thread::id runtime_io_owner;
aa::trust::PhoneIdentity runtime_phone;

class RuntimeTransport final : public aa::transport::Transport {
  public:
    explicit RuntimeTransport(bool drops) : drops_(drops) {
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::VersionResponse{1, 6, 0}).value()));
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::EncapsulatedSsl{{std::byte{1}}}).value()));
        frames_.push_back(wiredtest::open_request_frame(99));
    }
    aa::transport::Kind kind() const noexcept override { return aa::transport::Kind::usb_aoa; }
    aa::core::Result<void> open(aa::core::CancellationToken token) override {
        token_ = token;
        return {};
    }
    aa::core::Result<void> send(std::span<const std::byte>) override { return {}; }
    void set_io_checkpoint(std::function<void()> checkpoint) override {
        checkpoint_ = std::move(checkpoint);
    }
    aa::core::Result<std::vector<std::byte>> receive() override {
        {
            const std::lock_guard lock(runtime_owner_mutex);
            runtime_io_owner = std::this_thread::get_id();
        }
        if (checkpoint_) {
            checkpoint_();
        }
        if (next_ < frames_.size()) {
            return frames_[next_++];
        }
        if (drops_ && !runtime_quiet) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        runtime_waiting.store(true);
        while (!token_.stop_requested()) {
            lock.unlock();
            if (checkpoint_) {
                checkpoint_();
            }
            lock.lock();
            cv.wait_for(lock, token_, std::chrono::milliseconds{20}, [] { return false; });
        }
        close();
        return aa::Error{aa::ErrorCode::cancelled};
    }
    void close() noexcept override { runtime_closed.store(true); }
  private:
    bool drops_;
    std::size_t next_{};
    aa::core::CancellationToken token_;
    std::function<void()> checkpoint_;
    std::vector<std::vector<std::byte>> frames_;
};
}

// Link-time seams keep the real CLI/thread loop under test without USB or audio hardware.
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
                  const aa::trust::TrustStore &, const aa::core::Deadline &)
    asm("__wrap__ZN2aa3app15open_wired_linkERKNS_9transport17UsbDeviceSelectorERKNS1_16AoaAccessoryInfoERKNS_5trust10TrustStoreERKNS_4core8DeadlineE");
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
                  const aa::trust::TrustStore &, const aa::core::Deadline &) {
    const int attempt = ++runtime_connects;
    if (attempt == 1 || (runtime_recovers && attempt >= 4)) {
        return aa::app::WiredLink{std::make_unique<RuntimeTransport>(attempt == 1), runtime_phone};
    }
    return aa::Error{aa::ErrorCode::transport_unavailable};
}

std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &)
    asm("__wrap__ZN2aa9transport28make_client_handshake_cipherERKSt8functionIFbvEE");
std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &) {
    ++runtime_ciphers;
    return std::make_unique<wiredtest::FakeHandshakeCipher>();
}

aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &)
    asm("__wrap__ZN2aa5audio6detail17open_default_sinkERKNS1_15SinkOpenRequestE");
aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &) {
    return {std::make_unique<wiredtest::RecordingObservedSink>(
                std::make_shared<wiredtest::AudioObs>()), {}};
}

namespace {
class RejectAudio final : public aa::audio::detail::ObservedAudioSink {
  public:
    aa::core::Result<void> write(const aa::audio::MixedChunk &) override {
        return aa::Error{aa::ErrorCode::transport_io};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
};

TEST(WiredReceiver, FailedAudioPumpDoesNotEmitDelivery) {
    auto router = aa::audio::detail::open_router_with_sink_for_test(
        wiredtest::pcm_router_config(), std::make_unique<RejectAudio>());
    ASSERT_TRUE(router.has_value());
    RouterSink sink{router.value()};
    const auto pcm = wiredtest::pcm_unit();
    aa::protocol::MessageView message{
        {aa::protocol::ChannelRole::media_audio, 1, {}}, pcm};
    testing::internal::CaptureStdout();
    const auto result = sink.on_message(message);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(output.find("audio_chunks"), std::string::npos)
        << "failed pump emitted delivery evidence: " << output;
}

struct RuntimeResult {
    int code;
    std::string output;
    std::chrono::steady_clock::duration elapsed;
};

RuntimeResult run_runtime(bool recovers) {
    char directory[] = "/tmp/wired-runtime-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore store{path};
    EXPECT_TRUE(store.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    EXPECT_TRUE(store.approve(aa::trust::TransportIdentity{identity}).has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_recovers = recovers;
    runtime_quiet = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    g_running.store(true);
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--no-bus", "--store",
                                  path.string(), "--video-socket", (root / "video").string(),
                                  "--input-socket", (root / "input").string(), "--duration-ms",
                                  recovers ? "1000" : "6500"};
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    testing::internal::CaptureStdout();
    const auto began = std::chrono::steady_clock::now();
    const int code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    const auto elapsed = std::chrono::steady_clock::now() - began;
    auto output = testing::internal::GetCapturedStdout();
    std::filesystem::remove_all(root);
    return {code, std::move(output), elapsed};
}

TEST(WiredReceiver, TransientLinkLossRetriesUntilFreshSessionRecovers) {
    const auto result = run_runtime(true);
    EXPECT_EQ(result.code, 0) << result.output;
    EXPECT_GE(runtime_connects.load(), 4) << "reconnect stopped after one immediate attempt";
    EXPECT_EQ(runtime_ciphers.load(), 2);
    const auto active = result.output.find("\"to_state\":\"active\"");
    ASSERT_NE(active, std::string::npos) << result.output;
    EXPECT_NE(result.output.find("\"to_state\":\"active\"", active + 1), std::string::npos)
        << result.output;
}

TEST(WiredReceiver, PermanentLinkLossExitsNonzeroWithinWiredRecoveryBudget) {
    const auto result = run_runtime(false);
    EXPECT_EQ(result.code, 1) << "dead session returned clean success: " << result.output;
    EXPECT_GE(runtime_connects.load(), 3);
    EXPECT_EQ(runtime_ciphers.load(), 1);
    EXPECT_LT(result.elapsed, std::chrono::milliseconds{5500});
}

TEST(WiredReceiver, ForgetPhoneUsesOwnerQueueAndDisconnectsQuietSession) {
    namespace bus = aa::ipc::dbus_adapter;
    namespace faults = aa::trust::test::faults;
    char directory[] = "/tmp/wired-revoke-runtime-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore initial{path};
    ASSERT_TRUE(initial.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    const auto approved = initial.approve(aa::trust::TransportIdentity{identity});
    ASSERT_TRUE(approved.has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_quiet = true;
    runtime_recovers = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    runtime_waiting.store(false);
    runtime_closed.store(false);
    g_running.store(true);
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--store", path.string(),
        "--video-socket", (root / "video").string(), "--input-socket", (root / "input").string(),
        "--duration-ms", "5000"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    std::vector<std::thread::id> mutations;
    faults::arm(faults::Fault::none);
    faults::set_observer([&] { mutations.push_back(std::this_thread::get_id()); });
    testing::internal::CaptureStdout();
    int code = -1;
    std::thread::id dispatcher;
    std::thread receiver([&] {
        dispatcher = std::this_thread::get_id();
        code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    });
    std::atomic<bool> watchdog_fired{false};
    std::jthread watchdog([&](const std::stop_token& stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, std::chrono::seconds{3}, [] { return false; });
        if (!stop.stop_requested()) {
            watchdog_fired.store(true);
            g_running.store(false);
        }
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!runtime_waiting.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool was_quiet = runtime_waiting.load();
    auto client = bus::BusConnection::connect_session();
    aa::core::Result<bus::ParsedMessage> reply{aa::Error{aa::ErrorCode::internal}};
    bool registered = false;
    bool closed_at_reply = false;
    if (client && was_quiet) {
        const aa::ipc::BusName consumer{"org.custom.AndroidAutoReceiverTest.Consumer"};
        const aa::ipc::BusName service{"org.custom.AndroidAutoReceiver"};
        auto named = client.value().request_name(consumer);
        auto owner = client.value().get_name_owner(service);
        if (named && owner) {
            bus::MessageWriter registration;
            registration.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                .interface_name("org.custom.AndroidAutoReceiver1").member("RegisterConsumer")
                .body_signature("s").put_string(consumer.value);
            auto serial = client.value().send_message(bus::MessageType::method_call, registration);
            if (serial) {
                auto result = client.value().await_reply(serial.value(), owner.value());
                registered = result && result.value().type == bus::MessageType::method_return;
            }
            if (registered) {
                bus::MessageWriter forget;
                forget.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                    .interface_name("org.custom.AndroidAutoReceiver1").member("ForgetPhone")
                    .body_signature("t").put_u64(approved.value().value);
                auto sent = client.value().send_message(bus::MessageType::method_call, forget);
                if (sent) {
                    reply = client.value().await_reply(sent.value(), owner.value());
                    closed_at_reply = runtime_closed.load();
                }
            }
        }
    }
    if (!registered || !reply) {
        g_running.store(false);
    }
    receiver.join();
    watchdog.request_stop();
    watchdog.join();
    const auto output = testing::internal::GetCapturedStdout();
    faults::set_observer({});
    faults::disarm();
    EXPECT_FALSE(watchdog_fired.load()) << output;
    EXPECT_TRUE(was_quiet);
    EXPECT_TRUE(registered);
    ASSERT_TRUE(reply.has_value()) << output;
    EXPECT_EQ(reply.value().type, bus::MessageType::method_return);
    EXPECT_TRUE(closed_at_reply);
    EXPECT_EQ(code, 1) << output;
    EXPECT_EQ(runtime_connects.load(), 1);
    EXPECT_EQ(runtime_ciphers.load(), 1);
    EXPECT_NE(output.find("\"to_state\":\"failed\""), std::string::npos) << output;
    EXPECT_EQ(output.find("\"detail\":\"poll\""), std::string::npos) << output;
    ASSERT_FALSE(mutations.empty());
    {
        const std::lock_guard lock(runtime_owner_mutex);
        for (const auto thread : mutations) {
            EXPECT_EQ(thread, runtime_io_owner);
            EXPECT_NE(thread, dispatcher);
        }
    }
    aa::trust::ApprovedPhoneStore persisted{path};
    ASSERT_TRUE(persisted.load().has_value());
    EXPECT_EQ(persisted.lookup(runtime_phone), aa::trust::Decision::unknown);
    std::filesystem::remove_all(root);
    runtime_quiet = false;
}
} // namespace
