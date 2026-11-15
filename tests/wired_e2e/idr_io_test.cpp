// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../../src/transport/tls_adapter/TlsRecordCodec.hpp"
#include <aa/transport/UsbByteTransport.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include <thread>

namespace {
namespace tr = aa::transport;
namespace p = aa::protocol;
using Bytes = std::vector<std::byte>;
using Clock = aa::core::Deadline::Clock;
using Codec = tr::tls_adapter::TlsRecordCodec;
using namespace std::chrono_literals;

// Real USB byte framing, with a checkpoint between accepted partial writes.
class CheckpointEndpoint final : public tr::ByteEndpoint {
public:
    Bytes inbound, outbound;
    std::size_t chunk = 65536;
    std::function<void()> checkpoint, during_read;
    void set_io_checkpoint(std::function<void()> value) override { checkpoint = std::move(value); }
    Clock::time_point deadline{};
    aa::core::Result<void> open() override { return {}; }
    void set_io_deadline(Clock::time_point value) noexcept override { deadline = value; }
    aa::core::Result<std::size_t> read(std::span<std::byte> buffer,
                                     const aa::core::CancellationToken& token) override {
        const auto installed = token;
        if (during_read) during_read();
        EXPECT_EQ(token, installed);
        if (token.stop_requested()) return aa::Error{aa::ErrorCode::cancelled};
        const auto size = std::min(buffer.size(), inbound.size());
        std::copy_n(inbound.begin(), size, buffer.begin());
        inbound.erase(inbound.begin(), inbound.begin() + static_cast<std::ptrdiff_t>(size));
        return size;
    }
    aa::core::Result<std::size_t> write(std::span<const std::byte> data,
                                      const aa::core::CancellationToken& token) override {
        if (token.stop_requested()) return aa::Error{aa::ErrorCode::cancelled};
        const auto size = std::min(chunk, data.size());
        outbound.insert(outbound.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(size));
        if (checkpoint) checkpoint();
        return size;
    }
    void close() noexcept override {}
};

class WiredIdrIo : public ::testing::Test {
protected:
    aa::session::test::MapTrustStore trust;
    aa::session::SteadyClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    std::unique_ptr<tr::UsbByteTransport> transport;
    CheckpointEndpoint* endpoint{};
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    wiredtest::RecordingSink sink;
    Codec phone{aa::tls::Policy{[] { return true; }, aa::tls::Mode::encryption_only_compatibility}, Codec::Role::server};
    tr::FrameStreamParser parser;
    tr::MessageReassembler messages{&phone, tr::PlaintextAfterHandshake::allow};

    std::vector<tr::Message> drain() {
        std::vector<tr::Message> result;
        auto frames = parser.feed(endpoint->outbound);
        endpoint->outbound.clear();
        EXPECT_TRUE(frames);
        if (!frames) return result;
        for (const auto& frame : frames.value()) {
            auto message = messages.push(frame);
            EXPECT_TRUE(message) << aa::to_string(message.error().code());
            if (message && message.value()) result.push_back(std::move(*message.value()));
        }
        return result;
    }
    void enqueue(std::uint8_t channel, Bytes payload, bool encrypted = true) {
        auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(payload)},
            encrypted ? tr::Encryption::encrypted : tr::Encryption::plain, encrypted ? &phone : nullptr);
        ASSERT_TRUE(frames);
        for (const auto& frame : frames.value())
            endpoint->inbound.insert(endpoint->inbound.end(), frame.begin(), frame.end());
    }
    void receive(std::uint8_t channel, Bytes payload, bool encrypted = true) {
        enqueue(channel, std::move(payload), encrypted);
        ASSERT_TRUE(session.poll_transport());
    }
    void SetUp() override {
        auto owned = std::make_unique<CheckpointEndpoint>();
        endpoint = owned.get();
        auto made = tr::UsbByteTransport::over_endpoint(std::move(owned));
        ASSERT_TRUE(made);
        transport = std::move(made).value();
        const aa::trust::PhoneIdentity identity{"idr-io-synthetic"};
        trust.set(identity, aa::trust::Decision::approved);
        ASSERT_TRUE(session.configure(wiredtest::golden_services()));
        ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, sink));
        session.attach_transport(transport.get());
        session.attach_cipher(std::make_unique<Codec>(
            aa::tls::Policy{[] { return true; }, aa::tls::Mode::encryption_only_compatibility}, Codec::Role::client));
        ASSERT_TRUE(session.apply(aa::session::Event::start_discovery));
        ASSERT_TRUE(session.phone_discovered(identity));
        ASSERT_TRUE(session.apply(aa::session::Event::transport_ready));
        receive(0, p::encode(p::VersionResponse{1, 6, 0}).value(), false);
        for (unsigned round = 0; round < 16 && !session.authenticated(); ++round) {
            for (const auto& message : drain()) {
                auto ssl = p::decode_encapsulated_ssl(message.payload);
                if (ssl) phone.give_handshake(ssl.value().bytes);
            }
            ASSERT_TRUE(phone.step_handshake());
            auto output = phone.take_handshake();
            if (!output.empty()) receive(0, p::encode(p::EncapsulatedSsl{std::move(output)}).value(), false);
        }
        ASSERT_TRUE(session.authenticated());
        ASSERT_TRUE(phone.is_active());
        (void)drain();
        ASSERT_TRUE(session.announce_services());
        (void)drain();
        receive(99, p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value());
        (void)drain();
        receive(99, p::encode(p::AvSinkStart{7, 0}).value());
        (void)drain();
        ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
        ASSERT_TRUE(session.video_projecting());
    }
};

TEST_F(WiredIdrIo, RecoveryDuringResponseWritePreservesTlsAndPartialFrameOrder) {
    for (const auto chunk : {std::size_t{65536}, std::size_t{3}}) {
        SCOPED_TRACE(chunk);
        endpoint->chunk = chunk;
        bool requested = false;
        transport->set_io_checkpoint([&] {
            if (!session.outbound_in_progress()) return;
            if (chunk == 3 && endpoint->outbound.empty()) return;
            EXPECT_TRUE(session.outbound_in_progress());
            if (requested) return;
            requested = true;
            // Becomes pending only after the unrelated response was encoded and
            // bytes accepted. Repeated automatic/external requests coalesce.
            const auto before = endpoint->outbound.size();
            EXPECT_TRUE(session.request_video_idr());
            EXPECT_TRUE(session.request_video_idr(true));
            EXPECT_TRUE(session.service_pending_video_idr());
            EXPECT_EQ(endpoint->outbound.size(), before);
        });
        receive(0, {std::byte{0}, std::byte{18}, std::byte{8}, std::byte{1}});
        EXPECT_TRUE(requested);
        transport->set_io_checkpoint({});
        auto replies = drain();
        ASSERT_EQ(replies.size(), 1U);
        EXPECT_EQ(replies[0].payload,
            p::encode(p::AudioFocusNotification{p::AudioFocusState::gain, false}).value());
        EXPECT_FALSE(session.outbound_in_progress());
        ASSERT_TRUE(session.service_pending_video_idr());
        auto recovery = drain();
        ASSERT_EQ(recovery.size(), 2U);
        EXPECT_EQ(recovery[0].payload, p::encode(p::VideoFocusNotification{p::VideoFocusMode::native, true}).value());
        EXPECT_EQ(recovery[1].payload, p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value());
        ASSERT_TRUE(session.service_pending_video_idr());
        EXPECT_TRUE(drain().empty());

        requested = false;
        transport->set_io_checkpoint([&] {
            if (!session.outbound_in_progress()) return;
            EXPECT_TRUE(session.outbound_in_progress());
            requested = true;
            EXPECT_TRUE(session.request_video_idr());
            EXPECT_TRUE(session.service_pending_video_idr());
        });
        const Bytes large(40000, std::byte{0x42});
        ASSERT_TRUE(session.send_channel_payload(p::ChannelRole::video, large));
        transport->set_io_checkpoint({});
        EXPECT_TRUE(requested);
        auto multi = drain();
        ASSERT_EQ(multi.size(), 1U);
        EXPECT_EQ(multi[0].payload, large);
        ASSERT_TRUE(session.service_pending_video_idr());
        ASSERT_EQ(drain().size(), 1U);
    }
}

TEST_F(WiredIdrIo, SuccessfulIdleRecoveryRestoresReadContextAndLifecycleCancellation) {
    bool recovered = false;
    endpoint->during_read = [&] {
        if (recovered) return;
        recovered = true;
        const auto read_deadline = endpoint->deadline;
        EXPECT_EQ(read_deadline, Clock::time_point::max());
        EXPECT_TRUE(session.request_video_idr(true));
        EXPECT_EQ(endpoint->deadline, read_deadline);
        session.request_stop();
    };
    auto result = session.poll_transport();
    ASSERT_FALSE(result);
    EXPECT_TRUE(recovered);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled);
    ASSERT_EQ(drain().size(), 2U);
}

// Observe the transport-installed token as well as the deadline. Deliberate
// non-recovery nested sends exercise general I/O scoping, including early exits.
class ContextTransport final : public tr::Transport {
public:
    Clock::time_point deadline{};
    aa::core::CancellationToken token;
    std::function<aa::core::Result<void>()> on_send;
    std::function<void()> on_read;
    bool fail_open = false;
    aa::core::Result<void> open(aa::core::CancellationToken value) override {
        token = value;
        if (fail_open) { fail_open = false; return aa::Error{aa::ErrorCode::transport_io}; }
        return {};
    }
    void set_io_deadline(Clock::time_point value) noexcept override { deadline = value; }
    tr::Kind kind() const noexcept override { return tr::Kind::tcp; }
    aa::core::Result<void> send(std::span<const std::byte>) override { return on_send(); }
    aa::core::Result<Bytes> receive() override { on_read(); return aa::Error{aa::ErrorCode::cancelled}; }
    void close() noexcept override {}
};

TEST(WiredIdrContext, NestedWritesRestoreImmediateContextOnSuccessAndEarlyError) {
    for (const int early_error : {0, 1, 2}) {
        SCOPED_TRACE(early_error);
        aa::session::test::MapTrustStore trust;
        aa::session::SteadyClock clock;
        wiredtest::CaptureSink capture;
        aa::diagnostics::Diagnostics diagnostics{capture};
        ContextTransport transport;
        aa::app::WiredSession session{{trust, clock, diagnostics}};
        session.attach_transport(&transport);
        unsigned depth = 0;
        transport.on_read = [&] {
            const auto read_token = transport.token;
            const auto read_deadline = transport.deadline;
            transport.on_send = [&]() -> aa::core::Result<void> {
                if (depth++ == 0) {
                    const auto outer_token = transport.token;
                    const auto outer_deadline = transport.deadline;
                    EXPECT_NE(outer_token, read_token);
                    EXPECT_LT(outer_deadline, read_deadline);
                    transport.fail_open = early_error == 2;
                    const auto nested = session.send_version_request(1, 6);
                    EXPECT_EQ(nested.has_value(), !early_error);
                    EXPECT_EQ(transport.token, outer_token);
                    EXPECT_EQ(transport.deadline, outer_deadline);
                    return {};
                }
                EXPECT_NE(transport.token, read_token);
                if (early_error) return aa::Error{aa::ErrorCode::transport_io};
                return {};
            };
            EXPECT_TRUE(session.send_version_request(1, 6));
            EXPECT_EQ(transport.token, read_token);
            EXPECT_EQ(transport.deadline, read_deadline);
            session.request_stop();
            EXPECT_TRUE(transport.token.stop_requested());
        };
        EXPECT_FALSE(session.poll_transport());
        EXPECT_EQ(depth, early_error == 2 ? 1U : 2U);
    }
}

TEST(WiredIdrContext, ResumedOuterWriteKeepsItsOwnTimeoutAfterNestedError) {
    aa::session::test::MapTrustStore trust;
    aa::session::SteadyClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    ContextTransport transport;
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    session.attach_transport(&transport);
    unsigned depth = 0;
    transport.on_send = [&]() -> aa::core::Result<void> {
        if (depth++ != 0) return aa::Error{aa::ErrorCode::transport_io};
        const auto original_token = transport.token;
        const auto original_deadline = transport.deadline;
        EXPECT_FALSE(session.send_version_request(1, 6));
        EXPECT_EQ(transport.token, original_token);
        EXPECT_EQ(transport.deadline, original_deadline);
        // Mimic a transport wait: only the installed context can wake it.
        const auto watchdog = Clock::now() + 6s;
        while (!transport.token.stop_requested() && Clock::now() < watchdog)
            std::this_thread::sleep_for(1ms);
        EXPECT_TRUE(transport.token.stop_requested());
        return aa::Error{aa::ErrorCode::cancelled};
    };
    transport.on_read = [&] {
        const auto read_token = transport.token;
        const auto read_deadline = transport.deadline;
        const auto start = Clock::now();
        auto result = session.send_version_request(1, 6);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::timeout);
        EXPECT_LT(Clock::now() - start, 5500ms);
        EXPECT_EQ(transport.token, read_token);
        EXPECT_EQ(transport.deadline, read_deadline);
    };
    EXPECT_FALSE(session.poll_transport());
}
} // namespace
