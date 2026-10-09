// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 followup (defect 7): the handwritten JSON boundary must follow
// standard text-interchange rules (valid UTF-8, no unescaped control bytes)
// and export -> load -> export must be byte-symmetric for canonical quoting.

#include "tamper.hpp"

#include <aa/replay/Recorder.hpp>
#include <aa/replay/Schema.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace replaytest;
namespace replay = aa::replay;

std::vector<std::byte> export_or_fail(replay::Recorder& recorder) {
    auto exported = recorder.export_fixture();
    EXPECT_TRUE(exported);
    return exported.value().file_bytes;
}

// One session record whose synthetic phone key is the only payload of interest.
std::vector<std::byte> session_fixture(const std::string& phone_key) {
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected", "discovering", "connecting"};
    EXPECT_TRUE(recorder.set_expectation(expect));
    using aa::core::Nanoseconds;
    using replay::SessionOp;
    using replay::SessionRecord;
    EXPECT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    EXPECT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, phone_key}));
    return export_or_fail(recorder);
}

TEST(ReplaySyntax, CanonicalQuoteHandlingRoundTripsSymmetrically) {
    // Given: a synthetic phone key carrying quotes, backslash and UTF-8 text.
    const std::string tricky = "id\"quo\\te-\xC3\xBCn\xC3\xAF-yes";
    // When: the fixture round-trips export -> load -> export.
    const auto file = session_fixture(tricky);
    auto loaded = replay::load_fixture(file);
    ASSERT_TRUE(loaded);
    auto reexported = replay::export_fixture(loaded.value());
    // Then: every byte survives, including the escaped payload.
    ASSERT_TRUE(reexported);
    EXPECT_EQ(reexported.value().file_bytes, file);
    ASSERT_FALSE(loaded.value().records.empty());
    const auto& step = std::get<replay::SessionRecord>(loaded.value().records[1].body);
    EXPECT_EQ(step.phone_key, tricky);
}

TEST(ReplaySyntax, InvalidUtf8RejectedByParserBoundary) {
    // Given: a well-formed fixture whose body string carries a raw invalid byte.
    const auto file = session_fixture("phone-ok");
    const auto bad = tamper_resealed(file, R"("phone":"phone-ok")",
                                     std::string("\"phone\":\"phone-")
                                         + static_cast<char>(0xFF) + "\"");
    ASSERT_TRUE(bad);
    // When: the loader parses it.
    const auto loaded = replay::load_fixture(*bad);
    // Then: the JSON boundary rejects it with a typed error.
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplaySyntax, UnescapedControlByteRejectedByParserBoundary) {
    // Given: a well-formed fixture whose body string carries a raw control byte.
    const auto file = session_fixture("phone-ok");
    const auto bad = tamper_resealed(file, R"("phone":"phone-ok")",
                                     std::string("\"phone\":\"phone-")
                                         + static_cast<char>(0x01) + "\"");
    ASSERT_TRUE(bad);
    // When: the loader parses it.
    const auto loaded = replay::load_fixture(*bad);
    // Then: the JSON boundary rejects it with a typed error.
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplaySyntax, EscapedQuoteInsideStringStaysSingleString) {
    // Given: a body whose phone key contains an escaped quote sequence.
    const auto file = session_fixture("a\"b");
    auto loaded = replay::load_fixture(file);
    ASSERT_TRUE(loaded);
    // When: the stored bytes carry the escaped spelling, not a raw quote.
    const std::string text = to_text(file);
    EXPECT_NE(text.find("a\\\"b"), std::string::npos);
    // Then: the parsed key is the unescaped original.
    const auto& step = std::get<replay::SessionRecord>(loaded.value().records[1].body);
    EXPECT_EQ(step.phone_key, "a\"b");
}

} // namespace
