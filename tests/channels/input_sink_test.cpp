// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/InputSink.hpp>

#include <array>
#include <gtest/gtest.h>

namespace {
TEST(InputWire, FeedbackIsAcceptedAndCountedWithoutReply) {
    aa::channels::InputSink sink;
    const std::array payload{std::byte{0x80}, std::byte{0x04}};
    EXPECT_TRUE(sink.on_message({{aa::protocol::ChannelRole::input, 1, {}}, payload}));
    EXPECT_EQ(sink.feedback(), 1u);
    EXPECT_EQ(sink.unhandled(), 0u);
}

TEST(InputWire, UnknownAndShortPayloadsAreUnhandledWithoutFailure) {
    aa::channels::InputSink sink;
    const std::array payload{std::byte{0x80}, std::byte{0x02}, std::byte{0xff}};
    EXPECT_TRUE(sink.on_message({{aa::protocol::ChannelRole::input, 1, {}}, payload}));
    EXPECT_TRUE(sink.on_message({{aa::protocol::ChannelRole::input, 2, {}}, {}}));
    EXPECT_EQ(sink.feedback(), 0u);
    EXPECT_EQ(sink.unhandled(), 2u);
}

TEST(InputWire, WrongRoleRemainsRejected) {
    aa::channels::InputSink sink;
    EXPECT_FALSE(sink.on_message({{aa::protocol::ChannelRole::sensors, 1, {}}, {}}));
    EXPECT_EQ(sink.unhandled(), 0u);
}
}
