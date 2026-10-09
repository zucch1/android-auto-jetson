// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 followup (defect 4): the scripted replay transport honours the bound
// cancellation token on open and send, terminally, without advancing the
// script index or the matched-send counter.

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>

#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace replaytest;
namespace replay = aa::replay;

TEST(ReplayTransport, EndCancelSendMismatchTypedErrors) {
    using aa::ErrorCode;
    // Given: a scripted replay transport with one inbound frame.
    replay::ReplayTransport::Script script;
    script.inbound.push_back(make_bytes({0x01, 0x02, 0x03, 0x04}));
    replay::ReplayTransport transport(script);
    ASSERT_TRUE(transport.open(aa::core::CancellationToken{}));
    // When: the script drains and the peer hits end of fixture.
    auto frame = transport.receive();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame.value(), script.inbound.front());
    auto end = transport.receive();
    // Then: end of script is the typed transport_closed.
    ASSERT_FALSE(end);
    EXPECT_EQ(end.error().code(), ErrorCode::transport_closed);

    // Given: a bound cancellation token that fires.
    replay::ReplayTransport::Script cancelled_script;
    cancelled_script.inbound.push_back(make_bytes({0x01}));
    replay::ReplayTransport cancelled(cancelled_script);
    aa::core::CancellationSource source;
    ASSERT_TRUE(cancelled.open(source.get_token()));
    source.request_stop();
    // When/Then: receive reports the typed cancel error and closes.
    auto stopped = cancelled.receive();
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code(), ErrorCode::cancelled);
    auto after = cancelled.receive();
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code(), ErrorCode::transport_closed);

    // Given: an expected outbound frame in the script.
    replay::ReplayTransport::Script send_script;
    send_script.outbound.push_back(make_bytes({0x0A, 0x0B}));
    replay::ReplayTransport sender(send_script);
    ASSERT_TRUE(sender.open(aa::core::CancellationToken{}));
    // When/Then: a mismatched send is a typed error and terminally closes.
    auto mismatch = sender.send(make_bytes({0x09, 0x09}));
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code(), ErrorCode::invalid_argument);
    auto closed = sender.send(make_bytes({0x0A, 0x0B}));
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code(), ErrorCode::transport_closed);

    // Given: a fresh transport whose send matches.
    replay::ReplayTransport::Script matched_script;
    matched_script.outbound.push_back(make_bytes({0x0A, 0x0B}));
    replay::ReplayTransport matched(matched_script);
    ASSERT_TRUE(matched.open(aa::core::CancellationToken{}));
    ASSERT_TRUE(matched.send(make_bytes({0x0A, 0x0B})));
    // When/Then: an unexpected extra send is also the typed mismatch.
    auto extra = matched.send(make_bytes({0x0A, 0x0B}));
    ASSERT_FALSE(extra);
    EXPECT_EQ(extra.error().code(), ErrorCode::invalid_argument);
    EXPECT_EQ(matched_script.matched_sends, 1);
}


TEST(ReplayTransport, SendAfterCancelIsCancelledAndFrozen) {
    using aa::ErrorCode;
    // Given: a scripted outbound frame on a transport whose token fires.
    replay::ReplayTransport::Script script;
    script.outbound.push_back(make_bytes({0x0A, 0x0B}));
    replay::ReplayTransport transport(script);
    aa::core::CancellationSource source;
    ASSERT_TRUE(transport.open(source.get_token()));
    source.request_stop();
    // When: the matching frame is sent after cancellation.
    auto stopped = transport.send(make_bytes({0x0A, 0x0B}));
    // Then: the typed cancel error surfaces and nothing is consumed.
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code(), ErrorCode::cancelled);
    EXPECT_EQ(script.outbound_pos, 0u);
    EXPECT_EQ(script.matched_sends, 0);
}

TEST(ReplayTransport, OpenPreCancelledIsCancelled) {
    using aa::ErrorCode;
    // Given: a token that fires before the transport opens.
    replay::ReplayTransport::Script script;
    script.inbound.push_back(make_bytes({0x01}));
    replay::ReplayTransport transport(script);
    aa::core::CancellationSource source;
    source.request_stop();
    // When: open binds the already-cancelled token.
    auto opened = transport.open(source.get_token());
    // Then: the typed cancel error surfaces and the handle stays terminal.
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), ErrorCode::cancelled);
    auto served = transport.receive();
    ASSERT_FALSE(served);
    EXPECT_EQ(served.error().code(), ErrorCode::transport_closed);
    EXPECT_EQ(script.inbound_pos, 0u);
}

} // namespace
