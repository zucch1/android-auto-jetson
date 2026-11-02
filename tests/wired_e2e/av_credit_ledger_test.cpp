// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wired-receiver/AvCreditLedger.hpp"
#include <aa/channels/AvSink.hpp>
#include <gtest/gtest.h>

namespace {
using aa::channels::AvCreditEvent;
using aa::channels::AvObservation;
using aa::wired_receiver::AvCreditLedger;

AvObservation arrival(std::uint64_t sequence) {
    AvObservation value{};
    value.event = AvCreditEvent::arrival;
    value.delivery = {aa::protocol::ChannelRole::media_audio, 1, sequence, 7};
    value.message_sequence = sequence;
    value.bytes = 8202;
    return value;
}

TEST(AvCreditLedger, RetainsLast64EventsButGenerationTotalsIncludeEvictedArrivals) {
    // Given: a fixed ledger with 100 data arrivals in one generation.
    AvCreditLedger ledger;
    for (std::uint64_t i = 1; i <= 100; ++i) { ledger.record(arrival(i), {}); }
    // When: its retained window is captured.
    const auto snapshot = ledger.snapshot();
    // Then: only 64 events remain, but the current-generation count is complete.
    EXPECT_EQ(snapshot.size, 64U);
    EXPECT_EQ(snapshot.events.front().ordinal, 37U);
    EXPECT_EQ(snapshot.events.back().ordinal, 100U);
    EXPECT_EQ(snapshot.generations[0].arrivals[4], 100U);
    EXPECT_FALSE(ledger.frozen());
}

TEST(AvCreditLedger, FreezeOwnsWindowAndTotalsBeforeLaterAckAndGenerationReset) {
    // Given: the first rejected data delivery freezes a populated ledger.
    AvCreditLedger ledger;
    for (std::uint64_t i = 1; i <= 100; ++i) { ledger.record(arrival(i), {}); }
    auto rejection = arrival(101);
    rejection.event = AvCreditEvent::rejected;
    rejection.error = aa::ErrorCode::ipc_queue_full;
    ledger.record(rejection, {});
    const auto frozen = ledger.frozen();
    ASSERT_TRUE(frozen);
    // When: completions and a new generation arrive after rejection.
    auto suffix = arrival(102);
    suffix.event = AvCreditEvent::ack;
    ledger.record(suffix, {});
    suffix.event = AvCreditEvent::reset;
    suffix.delivery.generation = 2;
    ledger.record(suffix, {});
    // Then: neither the owned frozen window nor its counters changes.
    EXPECT_EQ(ledger.frozen(), frozen);
    EXPECT_EQ(ledger.snapshot(), *frozen);
    EXPECT_EQ(frozen->size, 64U);
    EXPECT_EQ(frozen->events.back().ordinal, 101U);
}

TEST(AvCreditLedger, NewGenerationKeepsPriorTotalsWithoutMixingCurrentCredits) {
    // Given: generation one has an ACKed data delivery.
    AvCreditLedger ledger;
    ledger.record(arrival(1), {});
    auto ack = arrival(1);
    ack.event = AvCreditEvent::ack;
    ack.count = 1;
    ledger.record(ack, {});
    auto next = arrival(2);
    next.delivery.generation = 2;
    // When: a new generation receives its first data.
    ledger.record(next, {});
    // Then: its counters are separate from the previous generation's credit.
    const auto snapshot = ledger.snapshot();
    EXPECT_EQ(snapshot.generations[0].generation, 2U);
    EXPECT_EQ(snapshot.generations[0].arrivals[4], 1U);
    EXPECT_EQ(snapshot.generations[0].data_acks, 0U);
    EXPECT_EQ(snapshot.previous_generations[0].generation, 1U);
    EXPECT_EQ(snapshot.previous_generations[0].data_acks, 1U);
}

TEST(AvCreditLedger, OptionalObservationPreservesAckBytesBodiesIdsAndPendingRejection) {
    // Given: the same protocol exchange with observation absent or enabled.
    struct Transcript {
        std::vector<std::vector<std::byte>> sent, bodies;
        std::vector<aa::channels::AvDeliveryId> ids;
        std::optional<aa::ErrorCode> rejection;
    };
    const auto exchange = [](bool observed) {
        Transcript transcript;
        aa::channels::DeferredAvDelivery delivery{
            [&](const auto& message, auto id) -> aa::core::Result<void> {
                transcript.bodies.emplace_back(message.payload.begin(), message.payload.end());
                transcript.ids.push_back(id);
                return {};
            }, [](auto, auto) {}};
        AvCreditLedger ledger;
        if (observed) { delivery.observe = [&](const auto& value) { ledger.record(value, {}); }; }
        const auto role = aa::protocol::ChannelRole::media_audio;
        aa::channels::AvSink sink{{aa::protocol::ServiceKey{5}, role,
            aa::protocol::AudioConfiguration{{{48000, 16, 2}}}}, std::move(delivery),
            [&](auto bytes) -> aa::core::Result<void> {
                transcript.sent.push_back(std::move(bytes));
                return {};
            }};
        const auto send = [&](const auto& payload) { return sink.on_message({{role, 42, {123}}, payload}); };
        EXPECT_TRUE(send(aa::protocol::encode(aa::protocol::AvSinkSetup{}).value()));
        EXPECT_TRUE(send(std::vector<std::byte>{std::byte{0}, std::byte{1}}));
        EXPECT_TRUE(send(aa::protocol::encode(aa::protocol::AvSinkStart{-7, 0}).value()));
        const std::vector<std::byte> pcm(14, std::byte{0});
        EXPECT_TRUE(send(pcm));
        const auto duplicate = send(pcm);
        if (!duplicate) { transcript.rejection = duplicate.error().code(); }
        if (!transcript.ids.empty()) { EXPECT_TRUE(sink.complete_av_delivery(transcript.ids.front(), {})); }
        EXPECT_TRUE(send(aa::protocol::encode(aa::protocol::AvSinkStop{}).value()));
        return transcript;
    };
    const auto baseline = exchange(false);
    // When: that exchange runs through the observational callback.
    const auto observed = exchange(true);
    // Then: protocol output, delivered media, identity and rejection are identical.
    EXPECT_EQ(observed.sent, baseline.sent);
    EXPECT_EQ(observed.bodies, baseline.bodies);
    EXPECT_EQ(observed.ids, baseline.ids);
    EXPECT_EQ(observed.rejection, baseline.rejection);
    EXPECT_EQ(observed.rejection, aa::ErrorCode::ipc_queue_full);
    EXPECT_EQ(observed.sent.size(), 3U);
    EXPECT_EQ(observed.ids.size(), 1U);
}
} // namespace
