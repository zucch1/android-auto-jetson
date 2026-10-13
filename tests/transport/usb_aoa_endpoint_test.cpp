// SPDX-License-Identifier: GPL-3.0-or-later
// AOA bulk ByteEndpoint tests (task 28): byte I/O over a scripted bulk pipe,
// the framed UsbByteTransport composition, malformed peer bytes, and
// cancellation of in-flight reads/writes with a clean close and a working
// fresh endpoint afterwards (cancel_resume class).

#include "../src/transport/usb_adapter/UsbAoaEndpoint.hpp"

#include "UsbAoaFakes.hpp"

#include <aa/core/Cancellation.hpp>
#include <aa/core/Error.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/UsbByteTransport.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

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
    fx::FakeLinkFactory *factory;
};

Connected connect(std::unique_ptr<fx::FakeLinkFactory> factory) {
    Connected out;
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
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
    ASSERT_TRUE(link.endpoint != nullptr);
    ASSERT_TRUE(link.factory->pipe() != nullptr);

    auto written = link.endpoint->write(pattern(64), {});
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written.value(), 64u);
    EXPECT_EQ(link.factory->pipe()->out_size(), 64u);

    link.factory->pipe()->push_in(pattern(32));
    std::array<std::byte, 64> buffer{};
    auto read = link.endpoint->read(buffer, {});
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value(), 32u);
}

TEST(UsbAoaEndpoint, OpenIsIdempotentAndCloseIsTerminal) {
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
    ASSERT_TRUE(link.endpoint != nullptr);
    EXPECT_TRUE(link.endpoint->open().has_value());
    EXPECT_EQ(link.factory->switch_calls, 1);
    EXPECT_EQ(link.factory->claim_calls, 1);

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
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
    ASSERT_TRUE(link.endpoint != nullptr);
    auto read = link.endpoint->read({}, {});
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value(), 0u);
    EXPECT_EQ(link.factory->pipe()->reads(), 0);
}

TEST(UsbAoaEndpoint, FramedRoundTripThroughUsbByteTransport) {
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
    ASSERT_TRUE(link.endpoint != nullptr);
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);

    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.endpoint));
    ASSERT_TRUE(transport.has_value());
    aa::core::CancellationSource source;
    ASSERT_TRUE(transport.value()->open(source.get_token()).has_value());
    EXPECT_EQ(transport.value()->kind(), tr::Kind::usb_aoa);

    // A framed message traverses the bulk OUT pipe as exactly one frame.
    const tr::FrameHeader header{0x00, tr::FrameType::bulk, tr::MessageKind::control,
                                 tr::Encryption::plain};
    auto frame = tr::encode_frame(header, pattern(6), 0);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(transport.value()->send(frame.value()).has_value());
    EXPECT_EQ(pipe->out(), frame.value());

    // A framed message from the phone is received intact from the bulk IN pipe.
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
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
    ASSERT_TRUE(link.endpoint != nullptr);
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.endpoint));
    ASSERT_TRUE(transport.has_value());
    aa::core::CancellationSource source;
    ASSERT_TRUE(transport.value()->open(source.get_token()).has_value());

    // Garbage frame bytes: reserved flags in the header.
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
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
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
    while (!started.load() || pipe->reads() == 0) {
        std::this_thread::yield();
    }
    source.request_stop();
    reader.join();
    EXPECT_FALSE(had_value);
    EXPECT_EQ(code, aa::ErrorCode::cancelled);

    // The cancelled endpoint is terminal; a fresh endpoint re-opens cleanly.
    auto reopened = link.endpoint->open();
    ASSERT_FALSE(reopened.has_value());
    EXPECT_EQ(reopened.error().code(), ErrorCode::transport_closed);
    Connected fresh = connect(std::make_unique<fx::FakeLinkFactory>());
    ASSERT_TRUE(fresh.endpoint != nullptr);
    auto written = fresh.endpoint->write(pattern(4), {});
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written.value(), 4u);
}

TEST(UsbAoaEndpoint, WriteIsCancelledInFlight) {
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
    ASSERT_TRUE(link.endpoint != nullptr);
    aa::core::CancellationSource source;
    source.request_stop();
    auto written = link.endpoint->write(pattern(16), source.get_token());
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), aa::ErrorCode::cancelled);
}

TEST(UsbAoaEndpoint, IdsAreCapturedForTheApprovedRecord) {
    Connected link = connect(std::make_unique<fx::FakeLinkFactory>());
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
