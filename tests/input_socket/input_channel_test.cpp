// SPDX-License-Identifier: GPL-3.0-or-later
// F3 regression (owner ruling 2026-10-10): key/D-pad AND mouse-to-touch events
// traverse the real input socket to a receiver-side endpoint arriving
// normalized (the todo-26 typed seam). Plus the gate-review F1 backpressure
// regressions: a stalled reader is typed congestion, counted, and never
// freezes or reconnect-churns the sender.
#include "pair.hpp"

#include <aa/ipc/InputChannel.hpp>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

TEST(InputChannel, KeyAndTouchEventsTraverseToEndpointNormalized) {
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    auto& sender = pair.value().sender;

    ASSERT_TRUE(sender.send(InputEvent{KeyEvent{19, true}}));
    ASSERT_TRUE(sender.send(InputEvent{KeyEvent{19, false}}));
    ASSERT_TRUE(sender.send(InputEvent{KeyEvent{29, true}}));
    ASSERT_TRUE(sender.send(InputEvent{TouchEvent{10, 20, TouchAction::down}}));
    ASSERT_TRUE(sender.send(InputEvent{TouchEvent{40, 50, TouchAction::move}}));
    ASSERT_TRUE(sender.send(InputEvent{TouchEvent{40, 50, TouchAction::up}}));

    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint) << "endpoint did not accept the display sender";
    ASSERT_EQ(sender.sent(), 6U);

    const auto expect = [&endpoint](const InputEvent& wanted) {
        auto got = endpoint.value().receive(aa::core::Milliseconds{2000});
        ASSERT_TRUE(got) << "endpoint missed a normalized event";
        EXPECT_EQ(got.value(), wanted);
    };
    expect(InputEvent{KeyEvent{19, true}});
    expect(InputEvent{KeyEvent{19, false}});
    expect(InputEvent{KeyEvent{29, true}});
    expect(InputEvent{TouchEvent{10, 20, TouchAction::down}});
    expect(InputEvent{TouchEvent{40, 50, TouchAction::move}});
    expect(InputEvent{TouchEvent{40, 50, TouchAction::up}});
    EXPECT_EQ(endpoint.value().rejected(), 0U);
}

TEST(InputChannel, KeyAndTouchDecodeKeepWireValues) {
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    ASSERT_TRUE(pair.value().sender.send(InputEvent{KeyEvent{4, true}}));
    ASSERT_TRUE(pair.value().sender.send(InputEvent{TouchEvent{1279, 719, TouchAction::move}}));
    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint);

    auto key = endpoint.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(key);
    const auto* key_event = std::get_if<KeyEvent>(&key.value());
    ASSERT_NE(key_event, nullptr);
    EXPECT_EQ(key_event->keycode, 4);
    EXPECT_TRUE(key_event->pressed);

    auto touch = endpoint.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(touch);
    const auto* touch_event = std::get_if<TouchEvent>(&touch.value());
    ASSERT_NE(touch_event, nullptr);
    EXPECT_EQ(touch_event->x, 1279U);
    EXPECT_EQ(touch_event->y, 719U);
    EXPECT_EQ(touch_event->action, TouchAction::move);
}

TEST(InputChannel, EndpointReceiveTimeoutIsTypedAndMutatesNothing) {
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    ASSERT_TRUE(pair.value().sender.send(InputEvent{KeyEvent{19, true}}));
    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint);
    auto first = endpoint.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(first);

    const auto idle = endpoint.value().receive(aa::core::Milliseconds{50});
    ASSERT_FALSE(idle);
    EXPECT_EQ(idle.error().code(), aa::ErrorCode::timeout);
    EXPECT_EQ(endpoint.value().rejected(), 0U);

    ASSERT_TRUE(pair.value().sender.send(InputEvent{KeyEvent{19, true}}));
    auto next = endpoint.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(next);
    EXPECT_EQ(next.value(), (InputEvent{KeyEvent{19, true}}));
}

TEST(InputChannel, MissingTargetSocketIsTransportUnavailable) {
    const std::string runtime = inputtest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = input_socket_path_in(runtime);
    InputSender sender;
    sender.attach(path, current_uid());

    const auto sent = sender.send(InputEvent{KeyEvent{19, true}});
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_unavailable);
    EXPECT_EQ(sender.sent(), 0U);
    EXPECT_EQ(sender.dropped(), 1U);
    ::rmdir(runtime.c_str());
}

TEST(InputChannel, DetachedSenderDropsAndCounts) {
    InputSender sender;
    EXPECT_TRUE(sender.closed());

    const auto sent = sender.send(InputEvent{TouchEvent{1, 2, TouchAction::down}});
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_closed);
    EXPECT_EQ(sender.sent(), 0U);
    EXPECT_EQ(sender.dropped(), 1U);
}

TEST(InputChannel, SenderReconnectsAfterEndpointRestart) {
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    const auto path = input_socket_path_in(pair.value().runtime_dir);

    ASSERT_TRUE(pair.value().sender.send(InputEvent{KeyEvent{19, true}}));
    {
        auto first = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
        ASSERT_TRUE(first);
        auto got = first.value().receive(aa::core::Milliseconds{2000});
        ASSERT_TRUE(got);
    }

    pair.value().server.close();
    auto restarted = InputServer::listen(path, InputServerOptions{current_uid()});
    ASSERT_TRUE(restarted) << "stale endpoint node was not cleared for re-listen";
    pair.value().server = std::move(restarted.value());

    ASSERT_TRUE(pair.value().sender.send(InputEvent{KeyEvent{20, true}}));
    auto second = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(second);
    auto got = second.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value(), (InputEvent{KeyEvent{20, true}}));
    EXPECT_EQ(pair.value().sender.dropped(), 0U);
}

TEST(InputChannel, StalledEndpointCongestionIsTypedCountedAndKeepsConnection) {
    // Given: a receiver-side endpoint that accepts but never reads.
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    auto& sender = pair.value().sender;
    ASSERT_TRUE(sender.send(InputEvent{KeyEvent{19, true}}));
    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint);
    const int handle_before = sender.native_handle();
    ASSERT_GE(handle_before, 0);

    // When: events pile up until the bounded queues fill.
    aa::core::Result<void> outcome{};
    std::size_t attempts = 0;
    for (; attempts < 20000; ++attempts) {
        outcome = sender.send(InputEvent{KeyEvent{19, true}});
        if (!outcome) {
            break;
        }
    }
    ASSERT_FALSE(outcome) << "bounded queues never filled against a stalled endpoint";

    // Then: the failure is the typed backpressure result and is counted.
    EXPECT_EQ(outcome.error().code(), aa::ErrorCode::transport_queue_full);
    EXPECT_EQ(sender.dropped(), 1U);
    const auto failure = sender.last_failure();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure.value(), aa::ErrorCode::transport_queue_full);

    // And: continued congestion keeps failing typed and counted while the
    // live connection is kept (flow control never reconnect-churns).
    for (int round = 0; round < 2; ++round) {
        auto again = sender.send(InputEvent{KeyEvent{20, true}});
        ASSERT_FALSE(again);
        EXPECT_EQ(again.error().code(), aa::ErrorCode::transport_queue_full);
    }
    EXPECT_EQ(sender.dropped(), 3U);
    EXPECT_EQ(sender.native_handle(), handle_before);
    auto second_connection = pair.value().server.accept_sender(aa::core::Milliseconds{0});
    ASSERT_FALSE(second_connection);
    EXPECT_EQ(second_connection.error().code(), aa::ErrorCode::timeout);

    // And: once the endpoint drains, delivery resumes on the same connection.
    while (true) {
        auto queued = endpoint.value().receive(aa::core::Milliseconds{20});
        if (!queued) {
            break;
        }
    }
    auto resumed = sender.send(InputEvent{KeyEvent{21, true}});
    ASSERT_TRUE(resumed);
    EXPECT_EQ(sender.native_handle(), handle_before);
    auto got = endpoint.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value(), (InputEvent{KeyEvent{21, true}}));
}

TEST(InputChannel, ConnectCongestionIsTypedAndCounted) {
    // Given: a listening endpoint whose accept queue is already full.
    const std::string runtime = inputtest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = input_socket_path_in(runtime);
    const auto slash = path.value.find_last_of('/');
    ASSERT_NE(slash, std::string::npos);
    ASSERT_EQ(::mkdir(path.value.substr(0, slash).c_str(), 0700), 0);
    const int listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.value.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);

    std::vector<int> clients{};
    bool queue_full = false;
    for (int index = 0; index < 64; ++index) {
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        ASSERT_GE(fd, 0);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            queue_full = errno == EAGAIN || errno == EWOULDBLOCK;
            ::close(fd);
            break;
        }
        clients.push_back(fd);
    }
    ASSERT_TRUE(queue_full) << "listen backlog never filled";

    // When: a sender connects while the accept queue is full.
    InputSender sender;
    sender.attach(path, current_uid());
    const auto sent = sender.send(InputEvent{KeyEvent{19, true}});

    // Then: connection establishment is bounded and typed, never blocking.
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_queue_full);
    EXPECT_EQ(sender.dropped(), 1U);
    EXPECT_EQ(sender.native_handle(), -1);
    const auto failure = sender.last_failure();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure.value(), aa::ErrorCode::transport_queue_full);

    for (const int fd : clients) {
        ::close(fd);
    }
    ::close(listener);
    static_cast<void>(::unlink(path.value.c_str()));
    static_cast<void>(::rmdir(path.value.substr(0, slash).c_str()));
    ::rmdir(runtime.c_str());
}
} // namespace
