// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include <gtest/gtest.h>

namespace wiredcontrol {
namespace proto = aa::protocol;
namespace ss = aa::session;
namespace tr = aa::transport;

class WiredControl : public ::testing::Test {
protected:
    ss::test::MapTrustStore trust;
    ss::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    wiredtest::FakeHandshakeCipher* cipher{nullptr};

    void SetUp() override {
        const aa::trust::PhoneIdentity phone{"control-test"};
        trust.set(phone, aa::trust::Decision::approved);
        ASSERT_TRUE(session.configure(wiredtest::golden_services()));
        session.attach_transport(&transport);
        auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
        cipher = owned.get();
        session.attach_cipher(std::move(owned));
        ASSERT_TRUE(session.apply(ss::Event::start_discovery));
        ASSERT_TRUE(session.phone_discovered(phone));
        ASSERT_TRUE(session.apply(ss::Event::transport_ready));
        wiredtest::walk_auth(session, transport);
        ASSERT_TRUE(session.authenticated());
    }

    aa::core::Result<bool> receive(std::vector<std::byte> payload) {
        auto frames = tr::encode_message({0, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) { return frames.error(); }
        for (auto& frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        return session.poll_transport();
    }

    void expect_ping() {
        const proto::PingRequest ping{12345, false, {std::byte{0x42}}};
        auto encoded = proto::encode(ping);
        ASSERT_TRUE(encoded);
        ASSERT_TRUE(receive(std::move(encoded).value()));
        const auto frame = tr::decode_frame(transport.last_sent());
        ASSERT_TRUE(frame);
        const auto reply = proto::decode_ping_response(frame.value().payload);
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply.value().timestamp, ping.timestamp);
        EXPECT_EQ(reply.value().data, ping.data);
        EXPECT_FALSE(session.failure());
        EXPECT_EQ(session.state(), ss::State::negotiating);
    }

    template<class Wire> void expect_encrypted_reply(std::uint8_t id, Wire& wire) {
        const auto frame = tr::decode_frame(transport.last_sent());
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame.value().header.channel, 0);
        EXPECT_EQ(frame.value().header.kind, tr::MessageKind::specific);
        EXPECT_EQ(frame.value().header.encryption, tr::Encryption::encrypted);
        auto payload = cipher->open(frame.value().payload);
        ASSERT_TRUE(payload);
        ASSERT_GE(payload.value().size(), 2u);
        EXPECT_EQ(payload.value()[0], std::byte{0});
        EXPECT_EQ(payload.value()[1], static_cast<std::byte>(id));
        ASSERT_TRUE(wire.ParseFromArray(payload.value().data() + 2,
                                       static_cast<int>(payload.value().size() - 2)));
    }

    void expect_modern_byebye(unsigned reason) {
        const auto before = transport.sent_count();
        ASSERT_TRUE(receive({std::byte{0}, std::byte{15}, std::byte{8}, static_cast<std::byte>(reason)}));
        ASSERT_EQ(transport.sent_count(), before + 1);
        const auto frame = tr::decode_frame(transport.last_sent());
        ASSERT_TRUE(frame);
        const auto payload = cipher->open(frame.value().payload);
        ASSERT_TRUE(payload);
        EXPECT_EQ(payload.value(), (std::vector<std::byte>{std::byte{0}, std::byte{16}}));
        EXPECT_EQ(session.state(), ss::State::stopping);
        EXPECT_FALSE(session.failure());
    }
};
} // namespace wiredcontrol
