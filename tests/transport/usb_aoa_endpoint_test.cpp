// SPDX-License-Identifier: GPL-3.0-or-later
// AOA bulk ByteEndpoint tests (task 28): byte I/O over a scripted bulk pipe,
// the framed UsbByteTransport composition, malformed peer bytes, and
// cancellation of in-flight reads/writes with a clean close and a working
// fresh endpoint afterwards (cancel_resume class). Gate P2-4 fixtures: a
// zero-progress bulk write is bounded to a typed transport_io failure, and
// cancellation fires WHILE a transfer is blocked mid-operation.
//
// All assertions read the test-owned shared observation block (UsbAoaFakes
// LinkState/PipeState), never fixture objects that the pipeline may destroy.

#include "../src/transport/usb_adapter/UsbAoaEndpoint.hpp"

#include "PendingIo.hpp"
#include "UsbAoaFakes.hpp"

#include <aa/core/Cancellation.hpp>
#include <aa/core/Error.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/UsbByteTransport.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sys/syscall.h>

namespace {

namespace tr = aa::transport;
namespace ua = aa::transport::usb_adapter;
namespace fx = aa::test::usb_aoa;
using aa::ErrorCode;

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 17u) & 0xFFu);
    }
    return out;
}

tr::WiredAdmission admit_all() {
    return [](const aa::trust::WiredIdentity &) { return true; };
}

struct Connected final {
    std::unique_ptr<ua::UsbAoaEndpoint> endpoint;
    std::shared_ptr<fx::LinkState> state;
    fx::FakeLinkFactory *factory{nullptr};
};

Connected connect() {
    Connected out;
    out.state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(out.state);
    out.factory = factory.get();
    auto endpoint = ua::UsbAoaEndpoint::make(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                             tr::AoaAccessoryInfo{}, admit_all());
    EXPECT_TRUE(endpoint.has_value());
    if (endpoint) {
        auto opened = endpoint.value()->open();
        EXPECT_TRUE(opened.has_value());
        out.endpoint = std::move(endpoint.value());
    }
    return out;
}

} // namespace

TEST(UsbAoaEndpoint, OpenRunsPipelineAndMovesBytes) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    auto pipe_state = link.state->pipe_state();
    ASSERT_TRUE(pipe_state != nullptr);

    auto written = link.endpoint->write(pattern(64), {});
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written.value(), 64u);
    EXPECT_EQ(pipe_state->out_size(), 64u);

    link.factory->pipe()->push_in(pattern(32));
    std::array<std::byte, 64> buffer{};
    auto read = link.endpoint->read(buffer, {});
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value(), 32u);
}

TEST(UsbAoaEndpoint, OpenIsIdempotentAndCloseIsTerminal) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    EXPECT_TRUE(link.endpoint->open().has_value());
    EXPECT_EQ(link.state->switches(), 1);
    EXPECT_EQ(link.state->claims(), 1);

    link.endpoint->close();
    link.endpoint->close();
    auto reopened = link.endpoint->open();
    ASSERT_FALSE(reopened.has_value());
    EXPECT_EQ(reopened.error().code(), ErrorCode::transport_closed);
    std::array<std::byte, 4> buffer{};
    auto read = link.endpoint->read(buffer, {});
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code(), ErrorCode::transport_closed);
    auto written = link.endpoint->write(buffer, {});
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), ErrorCode::transport_closed);
}

TEST(UsbAoaEndpoint, EmptyBufferReadReturnsZeroWithoutTouchingPipe) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    auto read = link.endpoint->read({}, {});
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value(), 0u);
    EXPECT_EQ(link.state->pipe_state()->reads_started.load(), 0);
}

TEST(UsbAoaEndpoint, FramedRoundTripThroughUsbByteTransport) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    auto pipe_state = link.state->pipe_state();
    ASSERT_TRUE(pipe_state != nullptr);
    // Driving handle grabbed while its owner is alive; observations go to the
    // shared block below.
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);

    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.endpoint));
    ASSERT_TRUE(transport.has_value());
    aa::core::CancellationSource source;
    ASSERT_TRUE(transport.value()->open(source.get_token()).has_value());
    EXPECT_EQ(transport.value()->kind(), tr::Kind::usb_aoa);

    const tr::FrameHeader header{0x00, tr::FrameType::bulk, tr::MessageKind::control,
                                 tr::Encryption::plain};
    auto frame = tr::encode_frame(header, pattern(6), 0);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(transport.value()->send(frame.value()).has_value());
    EXPECT_EQ(pipe_state->out_bytes(), frame.value());

    const tr::FrameHeader reply_header{0x00, tr::FrameType::bulk, tr::MessageKind::control,
                                       tr::Encryption::plain};
    auto reply = tr::encode_frame(reply_header, pattern(4), 0);
    ASSERT_TRUE(reply.has_value());
    pipe->push_in(reply.value());
    auto received = transport.value()->receive();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received.value(), reply.value());
}

TEST(UsbAoaEndpoint, MalformedFrameFromPhoneIsTypedErrorAndCloses) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.endpoint));
    ASSERT_TRUE(transport.has_value());
    aa::core::CancellationSource source;
    ASSERT_TRUE(transport.value()->open(source.get_token()).has_value());

    pipe->push_in(
        {std::byte{0x01}, std::byte{0xF3}, std::byte{0x00}, std::byte{0x01}, std::byte{0xAA}});
    auto received = transport.value()->receive();
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().code(), ErrorCode::transport_malformed_frame);
    auto after = transport.value()->receive();
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), ErrorCode::transport_closed);
}

TEST(UsbAoaEndpoint, ReadIsCancelledInFlightAndFreshEndpointWorks) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);
    aa::core::CancellationSource source;
    std::atomic<bool> started{false};
    bool had_value{true};
    aa::ErrorCode code{aa::ErrorCode::internal};
    std::thread reader([&] {
        std::array<std::byte, 8> buffer{};
        started.store(true);
        auto read = link.endpoint->read(buffer, source.get_token());
        had_value = read.has_value();
        if (!had_value) {
            code = read.error().code();
        }
    });
    while (!started.load() || link.state->pipe_state()->reads_started.load() == 0) {
        std::this_thread::yield();
    }
    source.request_stop();
    reader.join();
    EXPECT_FALSE(had_value);
    EXPECT_EQ(code, aa::ErrorCode::cancelled);

    auto reopened = link.endpoint->open();
    ASSERT_FALSE(reopened.has_value());
    EXPECT_EQ(reopened.error().code(), ErrorCode::transport_closed);
    Connected fresh = connect();
    ASSERT_TRUE(fresh.endpoint != nullptr);
    auto written = fresh.endpoint->write(pattern(4), {});
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written.value(), 4u);
}

TEST(UsbAoaEndpoint, WriteIsCancelledInFlight) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    aa::core::CancellationSource source;
    source.request_stop();
    auto written = link.endpoint->write(pattern(16), source.get_token());
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), aa::ErrorCode::cancelled);
}

TEST(UsbAoaEndpoint, ZeroProgressWriteIsBoundedToTypedFailure) {
    // Gate P2-4: a bulk OUT that makes no progress across bounded timeouts must
    // end in transport_io after 3 zero-progress attempts - never an endless
    // retry loop. The endpoint closes terminally on that failure.
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    link.factory->pipe()->zero_progress_writes = 1000;

    const auto began = std::chrono::steady_clock::now();
    auto written = link.endpoint->write(pattern(16), {});
    const auto elapsed = std::chrono::steady_clock::now() - began;

    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), ErrorCode::transport_io);
    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_EQ(link.state->pipe_state()->out_size(), 0u);
    auto after = link.endpoint->write(pattern(4), {});
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), ErrorCode::transport_closed);
}

TEST(UsbAoaEndpoint, CancelFiresWhileTransferIsBlockedMidOperation) {
    // Gate P2-4: the token fires WHILE the bulk transfer is blocked inside the
    // pipe (kernel-observed futex wait via PendingIo), not before it starts.
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    aa::core::CancellationSource source;
    std::atomic<bool> in_flight{false};
    bool had_value{true};
    aa::ErrorCode code{aa::ErrorCode::internal};
    PendingIo<void> pending([&] {
        std::array<std::byte, 8> buffer{};
        in_flight.store(true);
        auto read = link.endpoint->read(buffer, source.get_token());
        had_value = read.has_value();
        if (!had_value) {
            code = read.error().code();
        }
    });
    // Prove the worker is genuinely inside the blocking wait, then fire.
    EXPECT_TRUE(pending.blocked(SYS_futex));
    in_flight.store(true);
    source.request_stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pending.result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    }
    EXPECT_FALSE(had_value);
    EXPECT_EQ(code, aa::ErrorCode::cancelled);
}

TEST(UsbAoaEndpoint, CancelFiresWhileWriteIsBlockedMidOperation) {
    // Gate P2-4 write-path counterpart to the read fixture above: the token
    // fires WHILE the OUT transfer is blocked inside the pipe (kernel-observed
    // futex wait via PendingIo), not before it starts (the old pre-fired
    // WriteIsCancelledInFlight alone could not prove this).
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    link.factory->pipe()->block_writes = true;
    aa::core::CancellationSource source;
    bool had_value{true};
    aa::ErrorCode code{aa::ErrorCode::internal};
    PendingIo<void> pending([&] {
        auto written = link.endpoint->write(pattern(8), source.get_token());
        had_value = written.has_value();
        if (!had_value) {
            code = written.error().code();
        }
    });
    EXPECT_TRUE(pending.blocked(SYS_futex));
    source.request_stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pending.result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    }
    EXPECT_FALSE(had_value);
    EXPECT_EQ(code, aa::ErrorCode::cancelled);
}

TEST(UsbAoaEndpoint, IdsAreCapturedForTheApprovedRecord) {
    Connected link = connect();
    ASSERT_TRUE(link.endpoint != nullptr);
    EXPECT_EQ(link.endpoint->identity().usb_vendor_id, "04e8");
    EXPECT_EQ(link.endpoint->identity().usb_product_id, "6860");
    EXPECT_EQ(link.endpoint->identity().usb_serial, "TEST-SERIAL-001");
    EXPECT_EQ(link.endpoint->identity().aoa_manufacturer, "SAMSUNG");
    EXPECT_EQ(link.endpoint->identity().aoa_model, "SAMSUNG_Android");
    EXPECT_EQ(link.endpoint->identity().aoa_serial, "TEST-SERIAL-001");
    EXPECT_EQ(link.endpoint->identity().port_context, "1-2.1");
    EXPECT_EQ(link.endpoint->pre_switch().vendor_id, 0x04E8);
    EXPECT_EQ(link.endpoint->post_switch().vendor_id, 0x18D1);
    EXPECT_TRUE(link.endpoint->switch_report().started);
}

TEST(UsbAoaEndpoint, SlowAccessoryDiscoveryUsesCallerDeadline) {
    class SlowFactory final : public ua::AoaLinkFactory {
      public:
        void set_connect_deadline(const aa::core::Deadline &budget) override { budget_ = budget; }
        aa::core::Result<void> switch_to_accessory(
            const tr::UsbDeviceSelector &selector, const tr::AoaAccessoryInfo &info,
            tr::UsbDeviceSnapshot &pre, tr::AoaSwitchReport &report) override {
            return fake_.switch_to_accessory(selector, info, pre, report);
        }
        aa::core::Result<std::unique_ptr<ua::UsbBulkPipe>> claim_accessory(
            const tr::UsbDeviceSelector &, tr::UsbDeviceSnapshot &) override {
            std::this_thread::sleep_until(budget_.until);
            return aa::Error{aa::ErrorCode::timeout};
        }
      private:
        fx::FakeLinkFactory fake_;
        aa::core::Deadline budget_{
            std::chrono::steady_clock::now() + std::chrono::milliseconds{300}, {}};
    };
    auto endpoint = ua::UsbAoaEndpoint::make(std::make_unique<SlowFactory>(), {"1-2.1"}, {},
                                            admit_all());
    ASSERT_TRUE(endpoint.has_value());
    const auto began = std::chrono::steady_clock::now();
    endpoint.value()->set_io_deadline(began + std::chrono::milliseconds{30});
    const auto result = endpoint.value()->open();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::milliseconds{150})
        << "accessory discovery ignored the caller deadline";
}

TEST(UsbAoaEndpoint, PollIdleIsNonterminalAndNeverReplacesLifecycleDeadline) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    link.factory->pipe()->zero_progress_reads = 100;
    const auto lifecycle = aa::core::Deadline::Clock::now() + std::chrono::hours{1};
    link.endpoint->set_io_deadline(lifecycle);
    std::array<std::byte, 32> buffer{};
    for (int i = 0; i < 100; ++i) {
        auto idle = link.endpoint->poll_read(
            buffer, {}, aa::core::Deadline::Clock::now() + std::chrono::milliseconds{5});
        ASSERT_TRUE(idle.has_value());
        EXPECT_FALSE(idle.value().has_value());
        EXPECT_FALSE(state->closed);
    }
    EXPECT_GT(state->reads_started.load(), 0);
    EXPECT_GE(state->last_read_timeout_ms.load(), 1u);
    EXPECT_LE(state->last_read_timeout_ms.load(), 5u);
    link.factory->pipe()->push_in(pattern(8));
    auto got = link.endpoint->read(buffer, {});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got.value(), 8u);
    EXPECT_FALSE(state->closed);
}

TEST(UsbAoaEndpoint, ExpiredPollDoesNotTouchPipeButLaterActualDeadlineCloses) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    std::array<std::byte, 32> buffer{};
    link.endpoint->set_io_deadline(aa::core::Deadline::Clock::now() + std::chrono::hours{1});
    auto idle = link.endpoint->poll_read(buffer, {}, aa::core::Deadline::Clock::now() -
                                                    std::chrono::seconds{1});
    ASSERT_TRUE(idle.has_value());
    EXPECT_FALSE(idle.value().has_value());
    EXPECT_EQ(state->reads_started.load(), 0);
    EXPECT_FALSE(state->closed);
    link.factory->pipe()->push_in(pattern(4));
    auto still_live = link.endpoint->read(buffer, {});
    ASSERT_TRUE(still_live.has_value());
    EXPECT_EQ(still_live.value(), 4u);

    link.endpoint->set_io_deadline(aa::core::Deadline::Clock::now());
    auto expired = link.endpoint->poll_read(buffer, {}, aa::core::Deadline::Clock::now());
    ASSERT_FALSE(expired.has_value());
    EXPECT_EQ(expired.error().code(), ErrorCode::timeout);
    EXPECT_TRUE(state->closed);
    auto after = link.endpoint->open();
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), ErrorCode::transport_closed);
}

TEST(UsbAoaEndpoint, SubmillisecondPollNeverPassesInfiniteTimeoutToLibusb) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    std::array<std::byte, 32> buffer{};
    auto idle = link.endpoint->poll_read(buffer, {}, aa::core::Deadline::Clock::now() +
                                                    std::chrono::microseconds{500});
    ASSERT_TRUE(idle.has_value());
    EXPECT_FALSE(idle.value().has_value());
    EXPECT_EQ(state->reads_started.load(), 0);
    EXPECT_FALSE(state->closed);
}

TEST(UsbAoaEndpoint, BusyPartialFrameYieldsAfterOneTransferAndPreservesEveryByte) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    auto *pipe = link.factory->pipe();
    pipe->max_read_bytes = 1;
    auto frame = tr::encode_frame(
        {0x01, tr::FrameType::first, tr::MessageKind::specific, tr::Encryption::plain},
        pattern(16), 32);
    ASSERT_TRUE(frame.has_value());
    pipe->push_in(frame.value());
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.endpoint));
    ASSERT_TRUE(transport.has_value());
    int checkpoints = 0;
    transport.value()->set_io_checkpoint([&] { ++checkpoints; });
    for (std::size_t i = 0; i < frame.value().size(); ++i) {
        const auto reads_before = state->reads_started.load();
        auto got = transport.value()->poll_receive(aa::core::Deadline::Clock::now() +
                                                   std::chrono::seconds{1});
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(state->reads_started.load(), reads_before + 1);
        EXPECT_EQ(got.value().has_value(), i + 1 == frame.value().size());
        if (got.value()) {
            EXPECT_EQ(*got.value(), frame.value());
        }
        EXPECT_FALSE(state->closed);
    }
    EXPECT_EQ(checkpoints, static_cast<int>(frame.value().size()) * 4);
}

TEST(UsbAoaEndpoint, PollCheckpointCancellationAfterTransferRemainsTerminal) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    link.factory->pipe()->push_in(pattern(4));
    aa::core::CancellationSource source;
    int checkpoints = 0;
    link.endpoint->set_io_checkpoint([&] {
        if (++checkpoints == 2) {
            source.request_stop();
        }
    });
    std::array<std::byte, 32> buffer{};
    auto got = link.endpoint->poll_read(buffer, source.get_token(),
                                       aa::core::Deadline::Clock::now() + std::chrono::seconds{1});
    ASSERT_FALSE(got.has_value());
    EXPECT_EQ(got.error().code(), ErrorCode::cancelled);
    EXPECT_EQ(state->reads_started.load(), 1);
    EXPECT_TRUE(state->closed);
    EXPECT_FALSE(link.endpoint->open().has_value());
}

TEST(UsbAoaEndpoint, PollDetachClosesTransportAndPipe) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    link.factory->pipe()->detach();
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.endpoint));
    ASSERT_TRUE(transport.has_value());
    auto got = transport.value()->poll_receive(aa::core::Deadline::Clock::now() +
                                               std::chrono::seconds{1});
    ASSERT_FALSE(got.has_value());
    EXPECT_EQ(got.error().code(), ErrorCode::transport_closed);
    EXPECT_TRUE(state->closed);
    EXPECT_FALSE(transport.value()->open({}).has_value());
}

TEST(UsbAoaEndpoint, LegacyReadRetriesIdleTimeoutsInsteadOfReturningEof) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    link.factory->pipe()->zero_progress_reads = 3;
    link.factory->pipe()->push_in(pattern(8));
    std::array<std::byte, 32> buffer{};
    auto got = link.endpoint->read(buffer, {});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got.value(), 8u);
    EXPECT_EQ(state->reads_started.load(), 4);
    EXPECT_FALSE(state->closed);
}

TEST(UsbAoaEndpoint, PollActualLifecycleDeadlineClosesFramedTransportAfterIdle) {
    Connected link = connect();
    ASSERT_NE(link.endpoint, nullptr);
    auto state = link.state->pipe_state();
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.endpoint));
    ASSERT_TRUE(transport.has_value());
    auto idle = transport.value()->poll_receive(aa::core::Deadline::Clock::now());
    ASSERT_TRUE(idle.has_value());
    EXPECT_FALSE(idle.value().has_value());
    EXPECT_FALSE(state->closed);
    transport.value()->set_io_deadline(aa::core::Deadline::Clock::now());
    auto got = transport.value()->poll_receive(aa::core::Deadline::Clock::now() +
                                               std::chrono::seconds{1});
    ASSERT_FALSE(got.has_value());
    EXPECT_EQ(got.error().code(), ErrorCode::timeout);
    EXPECT_TRUE(state->closed);
    auto after = transport.value()->receive();
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), ErrorCode::transport_closed);
}
