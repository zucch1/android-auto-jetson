// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../../src/transport/tls_adapter/TlsRecordCodec.hpp"
#include <aa/protocol/WifiProjection.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/transport/TcpTransport.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include <thread>

namespace wireless_test {
namespace tr = aa::transport;
namespace proto = aa::protocol;
using Codec = tr::tls_adapter::TlsRecordCodec;
inline aa::core::Result<void> send(tr::TcpTransport& tcp, std::uint8_t channel,
                                 std::vector<std::byte> payload, tr::PayloadCipher* cipher = nullptr) {
    auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(payload)},
        cipher ? tr::Encryption::encrypted : tr::Encryption::plain, cipher);
    if (!frames) return frames.error();
    for (const auto& frame : frames.value()) {
        auto result = tcp.send(frame);
        if (!result) return result.error();
    }
    return {};
}
inline aa::core::Result<tr::Message> receive(tr::TcpTransport& tcp, tr::PayloadCipher* cipher = nullptr) {
    tr::MessageReassembler reassembler{cipher};
    for (;;) {
        auto frame = tcp.receive();
        if (!frame) return frame.error();
        auto message = reassembler.push(frame.value());
        if (!message) return message.error();
        if (message.value()) return std::move(*message.value());
    }
}
inline void walk_phone(tr::TcpTransport& tcp, const std::function<bool()>& receiver_active = {}) {
    Codec cipher{aa::tls::Policy{[] { return true; }, aa::tls::Mode::encryption_only_compatibility}, Codec::Role::server};
    auto version = receive(tcp);
    ASSERT_TRUE(version);
    ASSERT_TRUE(proto::decode_version_request(version.value().payload));
    ASSERT_TRUE(send(tcp, 0, proto::encode(proto::VersionResponse{1, 6, 0}).value()));
    bool authenticated = false;
    for (int round = 0; round < 16 && !authenticated; ++round) {
        auto message = receive(tcp);
        ASSERT_TRUE(message);
        if (auto auth = proto::decode_auth_complete(message.value().payload); auth) {
            ASSERT_TRUE(cipher.is_active());
            authenticated = true;
        } else {
            auto ssl = proto::decode_encapsulated_ssl(message.value().payload);
            ASSERT_TRUE(ssl);
            cipher.give_handshake(ssl.value().bytes);
            ASSERT_TRUE(cipher.step_handshake());
            auto output = cipher.take_handshake();
            if (!output.empty()) {
                ASSERT_TRUE(send(tcp, 0, proto::encode(proto::EncapsulatedSsl{std::move(output)}).value()));
            }
        }
    }
    ASSERT_TRUE(authenticated);
    ASSERT_TRUE(send(tcp, 0, {std::byte{0x00}, std::byte{0x05}}, &cipher));
    auto discovery = receive(tcp, &cipher);
    ASSERT_TRUE(discovery);
    auto services = proto::decode_service_discovery_response(discovery.value().payload);
    ASSERT_TRUE(services);
    ASSERT_EQ(services.value().services.size(), 4U);
    const auto wifi = std::find_if(services.value().services.begin(), services.value().services.end(),
        [](const auto& descriptor) { return descriptor.role == proto::ChannelRole::wifi_projection; });
    ASSERT_NE(wifi, services.value().services.end());
    EXPECT_EQ(wifi->id, proto::ServiceKey{73});
    ASSERT_TRUE(send(tcp, 0, proto::encode(proto::ChannelOpenRequest{0, proto::ServiceKey{73}}).value(), &cipher));
    ASSERT_TRUE(receive(tcp, &cipher));
    ASSERT_TRUE(send(tcp, 73, {std::byte{0x80}, std::byte{0x01}}, &cipher));
    auto credentials = receive(tcp, &cipher);
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials.value().channel, 73);
    EXPECT_EQ(credentials.value().payload,
        proto::encode(proto::WifiCredentialsResponse{proto::WifiSecurityMode::wpa2_personal,
            {36}, proto::WifiAccessPointType::dynamic}).value());
    if (receiver_active) {
        for (int round = 0; round < 100 && !receiver_active(); ++round) {
            ASSERT_TRUE(send(tcp, 0, proto::encode(proto::PingRequest{round, false, {}}).value(), &cipher));
            auto ping = receive(tcp, &cipher);
            ASSERT_TRUE(ping);
            ASSERT_TRUE(proto::decode_ping_response(ping.value().payload));
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ASSERT_TRUE(receiver_active());
    }
    ASSERT_TRUE(send(tcp, 99, {std::byte{0x10}, std::byte{0x20}}, &cipher));
    ASSERT_TRUE(send(tcp, 100, {std::byte{0x30}, std::byte{0x40}}, &cipher));
    auto input = receive(tcp, &cipher);
    ASSERT_TRUE(input);
    EXPECT_EQ(input.value().channel, 104);
    ASSERT_GE(input.value().payload.size(), 2U);
    EXPECT_EQ(input.value().payload[0], std::byte{0x80});
    EXPECT_EQ(input.value().payload[1], std::byte{0x01});
}
}
