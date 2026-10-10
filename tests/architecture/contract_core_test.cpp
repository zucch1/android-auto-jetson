// SPDX-License-Identifier: GPL-3.0-or-later
// Core contract smoke (task 14): typed errors, std::variant Result, strong ids
// and time units, the single-owner session thread, and the logging/redaction
// boundaries are usable exactly as documented.
#include <aa/core/Error.hpp>
#include <aa/core/Ids.hpp>
#include <aa/core/Logging.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/SessionThread.hpp>
#include <aa/core/Time.hpp>
#include <aa/diagnostics/Diagnostics.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct CapturingSink final : aa::core::LogSink {
    std::vector<aa::core::LogEvent> events;
    void write(const aa::core::LogEvent& event) override { events.push_back(event); }
};

TEST(CoreError, MapsStableCodesToDomainsAndMessages) {
    // Given: typed codes drawn from four module bands.
    const std::array<std::pair<aa::ErrorCode, aa::ErrorDomain>, 4> table{{
        {aa::ErrorCode::timeout, aa::ErrorDomain::core},
        {aa::ErrorCode::transport_oversize_frame, aa::ErrorDomain::transport},
        {aa::ErrorCode::session_unknown_phone, aa::ErrorDomain::session},
        {aa::ErrorCode::helper_rejected, aa::ErrorDomain::helper},
    }};
    // When: each is wrapped in the project error type.
    // Then: it maps to its module domain and carries stable names.
    for (const auto& [code, domain] : table) {
        const aa::Error error{code};
        EXPECT_EQ(error.code(), code);
        EXPECT_EQ(error.domain(), domain);
        EXPECT_FALSE(error.message().empty());
        EXPECT_FALSE(aa::to_string(code).empty());
    }
    EXPECT_EQ(aa::to_string(aa::ErrorDomain::protocol), "protocol");
}

TEST(CoreResult, HoldsExactlyOneOfValueOrTypedError) {
    // Given: results constructed on each variant alternative.
    aa::core::Result<int> good{7};
    aa::core::Result<int> bad{aa::Error{aa::ErrorCode::timeout}};
    // When/Then: accessors expose the stored alternative and nothing else.
    ASSERT_TRUE(good.has_value());
    EXPECT_EQ(good.value(), 7);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), aa::ErrorCode::timeout);
    EXPECT_EQ(bad.error().domain(), aa::ErrorDomain::core);
}

TEST(CoreResult, VoidResultTracksFailureOnly) {
    // Given: a success and a failure of the void specialization.
    aa::core::Result<void> good{};
    aa::core::Result<void> bad{aa::Error{aa::ErrorCode::cancelled}};
    // When/Then: both behave as variant alternatives.
    EXPECT_TRUE(good.has_value());
    EXPECT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), aa::ErrorCode::cancelled);
}

TEST(CoreIds, CompareByValueInsideOneIdSpace) {
    // Given: two ids of the same space and the unset sentinel.
    const aa::core::SessionId low{7};
    const aa::core::SessionId high{8};
    // When/Then: equality and ordering follow the value; zero is unset.
    EXPECT_TRUE(low == low);
    EXPECT_FALSE(low == high);
    EXPECT_TRUE(low < high);
    EXPECT_TRUE(static_cast<bool>(low));
    EXPECT_FALSE(static_cast<bool>(aa::core::SessionId{0}));
}

TEST(CoreTime, ConvertsBetweenStrongUnits) {
    // Given: a contract budget in milliseconds.
    constexpr aa::core::Milliseconds budget{33};
    // When: it is converted down to nanoseconds and floored back.
    const auto nanos = aa::core::to_nanoseconds(budget);
    // Then: the strong units round-trip without mixing.
    EXPECT_EQ(nanos.count, 33'000'000);
    EXPECT_EQ(aa::core::to_milliseconds_floor(nanos), budget);
    EXPECT_EQ(aa::core::to_milliseconds_floor(aa::core::Nanoseconds{33'999'999}), budget);
}

TEST(SessionThread, RunsPostedTasksSerializedOnOwnerThread) {
    // Given: a session thread and a probe of thread identity plus overlap.
    aa::core::SessionThread session;
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<bool> ran_elsewhere{false};
    std::atomic<int> runs{0};
    std::promise<void> both_ran;
    auto done = both_ran.get_future();
    auto probe = [&] {
        const int now = ++concurrent;
        int seen = max_concurrent.load();
        while (now > seen && !max_concurrent.compare_exchange_weak(seen, now)) {
        }
        if (!session.is_owner_thread()) {
            ran_elsewhere = true;
        }
        --concurrent;
        if (++runs == 2) {
            both_ran.set_value();
        }
    };
    // When: two foreign threads each post one task.
    std::atomic<bool> posted_first{false};
    std::atomic<bool> posted_second{false};
    std::thread first([&] { posted_first = session.post(probe); });
    std::thread second([&] { posted_second = session.post(probe); });
    first.join();
    second.join();
    // Then: both ran, strictly serialized, on the owner thread only.
    EXPECT_EQ(done.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    EXPECT_TRUE(posted_first.load());
    EXPECT_TRUE(posted_second.load());
    EXPECT_FALSE(ran_elsewhere.load());
    EXPECT_EQ(max_concurrent.load(), 1);
}

TEST(SessionThread, RejectsPostsAfterCancellationAndSignalsToken) {
    // Given: a live session thread with its cancellation token observed.
    aa::core::SessionThread session;
    const auto token = session.token();
    EXPECT_FALSE(token.stop_requested());
    // When: cancellation is requested and a task is posted afterwards.
    session.request_stop();
    std::atomic<bool> ran{false};
    // Then: the token fired and the dropped task never runs.
    EXPECT_TRUE(token.stop_requested());
    EXPECT_TRUE(session.stopped());
    EXPECT_FALSE(session.post([&] { ran = true; }));
    EXPECT_FALSE(ran.load());
}

TEST(Logging, FiltersBelowMinimumAndDeliversRedactedFields) {
    // Given: a capturing sink behind a logger with a minimum level.
    CapturingSink sink;
    const aa::core::Logger logger{sink, aa::core::LogLevel::info};
    // When: events at both sides of the threshold are logged with fields.
    logger.log(aa::core::LogLevel::debug, "dropped-by-level");
    logger.log(aa::core::LogLevel::info, "session-state", {{"session", "s-1"}});
    // Then: only the at-or-above event reaches the sink, with its fields intact.
    ASSERT_EQ(sink.events.size(), std::size_t{1});
    EXPECT_EQ(sink.events[0].message, "session-state");
    ASSERT_EQ(sink.events[0].fields.size(), std::size_t{1});
    EXPECT_EQ(sink.events[0].fields[0].key, "session");
    EXPECT_EQ(sink.events[0].fields[0].value, "s-1");
}

TEST(Diagnostics, RedactionHidesRawIdentifierBehindStablePseudonym) {
    // Given: a raw identifier that must never reach logs or events.
    const std::string raw{"AA:BB:CC:DD:EE:FF"};
    // When: the privacy boundary redacts it twice in one process.
    const auto first = aa::diagnostics::redact_identifier(raw);
    const auto second = aa::diagnostics::redact_identifier(raw);
    // Then: the pseudonym is stable within the run and hides the raw value.
    EXPECT_EQ(first, second);
    EXPECT_TRUE(first.rfind("id-", 0) == 0);
    EXPECT_EQ(first.find(raw), std::string::npos);
    EXPECT_TRUE(aa::diagnostics::redact_identifier(std::string_view{}).empty());
}

} // namespace
