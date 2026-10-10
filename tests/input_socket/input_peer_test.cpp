// SPDX-License-Identifier: GPL-3.0-or-later
// F3 regression (owner ruling 2026-10-10): SO_PEERCRED same-UID admission
// mirrors the video socket - unauthorized peers are rejected typed and
// counted, the listener stays up.
#include "pair.hpp"

#include <aa/ipc/InputChannel.hpp>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

TEST(InputPeer, ServerRejectsDifferentUidSender) {
    const std::string runtime = inputtest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = input_socket_path_in(runtime);
    auto server = InputServer::listen(path, InputServerOptions{current_uid() + 1});
    ASSERT_TRUE(server);

    InputSender sender;
    sender.attach(path, current_uid());
    const auto sent = sender.send(InputEvent{KeyEvent{19, true}});
    ASSERT_TRUE(sent) << "sender-side gate checks the server uid only, which matches";

    auto rejected = server.value().accept_sender(aa::core::Milliseconds{2000});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(server.value().unauthorized_peers(), 1U);
    EXPECT_FALSE(server.value().closed());
    server.value().close();
    ::rmdir(runtime.c_str());
}

TEST(InputPeer, SenderRejectsDifferentUidServer) {
    const std::string runtime = inputtest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = input_socket_path_in(runtime);
    auto server = InputServer::listen(path, InputServerOptions{current_uid()});
    ASSERT_TRUE(server);

    InputSender sender;
    sender.attach(path, current_uid() + 1);
    const auto sent = sender.send(InputEvent{KeyEvent{19, true}});
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(sender.sent(), 0U);

    server.value().close();
    ::rmdir(runtime.c_str());
}

TEST(InputPeer, SameUidPairExchangesAndCountsNobodyUnauthorized) {
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    ASSERT_TRUE(pair.value().sender.send(InputEvent{TouchEvent{7, 8, TouchAction::up}}));
    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint);
    auto got = endpoint.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value(), (InputEvent{TouchEvent{7, 8, TouchAction::up}}));
    EXPECT_EQ(pair.value().server.unauthorized_peers(), 0U);
}

TEST(InputPeer, MissingSocketIsTransportUnavailable) {
    const std::string runtime = inputtest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    InputSender sender;
    sender.attach(input_socket_path_in(runtime), current_uid());
    const auto sent = sender.send(InputEvent{KeyEvent{19, true}});
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_unavailable);
    ::rmdir(runtime.c_str());
}
} // namespace
