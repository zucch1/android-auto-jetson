// SPDX-License-Identifier: GPL-3.0-or-later
// Task 19 correction tests: the privacy gaps reproduced against the old
// serializer (unlabelled value on a sensitive key, numeric location/identifier
// variants, opaque identifier in an unknown field, reserved envelope keys
// overridden via duplicate keys, invalid UTF-8 breaking the always-valid-JSON
// claim) plus the diagnostics-owned safe core::LogSink adapter. Each case is
// the leak repro locked down at the corrected boundary.
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/diagnostics/SafeLogSink.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace diag = aa::diagnostics;

// In-memory fake of a backend sink: owns copies of what write() observed
// (LogEvent.message is a view valid only during the call, like core::Logger's).
struct CaptureLogSink final : aa::core::LogSink {
    struct Captured final {
        aa::core::LogLevel level;
        std::string message;
        std::vector<aa::core::LogField> fields;
    };
    std::vector<Captured> events;
    void write(const aa::core::LogEvent& event) override {
        events.push_back(Captured{event.level, std::string{event.message}, event.fields});
    }
};

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

std::size_t count_occurrences(std::string_view hay, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

TEST(DiagnosticsPrivacyGaps, SensitiveKeyFieldRedactsUnlabelledPlainValue) {
    // Given: a sensitive key whose value carries no recognizable secret shape
    // (the repro EventField::text("password","unlabelled-secret")).
    diag::Event event{};
    event.kind = diag::EventKind::pairing_requested;
    event.fields.push_back(diag::EventField::text("password", "unlabelled-secret"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: the unlabelled secret cannot survive on a sensitive key.
    EXPECT_EQ(out.find("unlabelled-secret"), std::string::npos);
    EXPECT_TRUE(contains(out, "\"password\":\"[redacted]\""));
}

TEST(DiagnosticsPrivacyGaps, TypedSensitiveFieldsHideEveryValueVariant) {
    // Given: typed location/credential/identifier fields carrying numeric and
    // boolean variants instead of strings (the variant-bypass repro).
    diag::Event event{};
    event.kind = diag::EventKind::transport_stats;
    event.fields.push_back(
        diag::EventField{"gps", diag::Sensitivity::location, 37.7749});
    event.fields.push_back(
        diag::EventField{"psk", diag::Sensitivity::credential, std::int64_t{424242}});
    event.fields.push_back(
        diag::EventField{"secret_flag", diag::Sensitivity::credential, true});
    event.fields.push_back(
        diag::EventField{"imei", diag::Sensitivity::identifier, std::int64_t{123456789012345}});
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: every variant is hidden; identifiers pseudonymize, secrets drop.
    EXPECT_TRUE(contains(out, "\"gps\":\"[redacted]\""));
    EXPECT_TRUE(contains(out, "\"psk\":\"[redacted]\""));
    EXPECT_TRUE(contains(out, "\"secret_flag\":\"[redacted]\""));
    EXPECT_TRUE(contains(out, "\"imei\":\"id-"));
    EXPECT_EQ(out.find("37.7749"), std::string::npos);
    EXPECT_EQ(out.find("424242"), std::string::npos);
    EXPECT_EQ(out.find("123456789012345"), std::string::npos);
    EXPECT_EQ(out.find("\"secret_flag\":true"), std::string::npos);
}

TEST(DiagnosticsPrivacyGaps, UnknownKeysAndValuesAreDroppedByDefault) {
    // Given: opaque identifiers and a numeric location under keys the safe
    // telemetry schema does not know.
    diag::Event event{};
    event.kind = diag::EventKind::helper_call;
    event.fields.push_back(diag::EventField::text("opaque_id", "8F3A22B19C0D77EE"));
    event.fields.push_back(diag::EventField::count("peer", 123456789012345));
    event.fields.push_back(diag::EventField::real("lat", 37.7749));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: nothing survives on an unknown key, string or scalar alike.
    EXPECT_EQ(out.find("opaque_id"), std::string::npos);
    EXPECT_EQ(out.find("\"peer\""), std::string::npos);
    EXPECT_EQ(out.find("\"lat\""), std::string::npos);
    EXPECT_EQ(out.find("8F3A22B19C0D77EE"), std::string::npos);
    EXPECT_EQ(out.find("123456789012345"), std::string::npos);
    EXPECT_EQ(out.find("37.7749"), std::string::npos);
}

TEST(DiagnosticsPrivacyGaps, ReservedEnvelopeKeysCannotBeOverriddenByFields) {
    // Given: dynamic fields carrying the reserved envelope key names with
    // hostile values (the duplicate-key override repro).
    diag::Event event{};
    event.session = aa::core::SessionId{9};
    event.kind = diag::EventKind::reconnect;
    event.monotonic_time = aa::core::Nanoseconds{7};
    event.fields.push_back(diag::EventField::text("kind", "evil"));
    event.fields.push_back(diag::EventField::text("session", "999"));
    event.fields.push_back(diag::EventField::text("mono_ns", "1"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: exactly one envelope key each, typed values, hostile ones dropped.
    EXPECT_EQ(count_occurrences(out, "\"kind\":"), std::size_t{1});
    EXPECT_EQ(count_occurrences(out, "\"session\":"), std::size_t{1});
    EXPECT_EQ(count_occurrences(out, "\"mono_ns\":"), std::size_t{1});
    EXPECT_TRUE(contains(out, "\"kind\":\"reconnect\""));
    EXPECT_TRUE(contains(out, "\"session\":9"));
    EXPECT_TRUE(contains(out, "\"mono_ns\":7"));
    EXPECT_EQ(out.find("evil"), std::string::npos);
    EXPECT_EQ(out.find("999"), std::string::npos);
}

TEST(DiagnosticsPrivacyGaps, InvalidUtf8RepairsInScrubAndFailsClosedAtEmission) {
    // Given: invalid UTF-8 (a lone 0xFF byte and a truncated 2-byte sequence)
    // inside a freeform value, plus an invalid byte in a key.
    std::string bad_value = "tail-";
    bad_value.push_back('\xFF');
    bad_value += "-head";
    bad_value.append("\xE2\x82");
    std::string bad_key = "k";
    bad_key.push_back('\xFF');
    bad_key += "y";
    diag::Event event{};
    event.kind = diag::EventKind::helper_call;
    event.fields.push_back(diag::EventField::text("reason", bad_value));
    event.fields.push_back(diag::EventField::text(bad_key, "x"));
    // When: the pattern layer repairs the text and emission rejects freeform values.
    const std::string repaired = diag::scrub_text(bad_value);
    const std::string out = diag::to_json(event);
    // Then: repair stays available, but no unvalidated content crosses emission.
    for (const char c : out) {
        EXPECT_NE(static_cast<unsigned char>(c), 0xFFU);
    }
    EXPECT_TRUE(contains(repaired, "\xEF\xBF\xBD"));
    EXPECT_TRUE(contains(repaired, "tail-"));
    EXPECT_TRUE(contains(repaired, "-head"));
    EXPECT_EQ(out.find("tail-"), std::string::npos);
    EXPECT_EQ(out.find("-head"), std::string::npos);
    EXPECT_TRUE(contains(out, "\"[redacted]\""));
}

TEST(DiagnosticsSafeLogSink, RedactsMessagesAndFieldsBeforeDownstreamObservesThem) {
    // Given: a logger whose sink is the safe adapter in front of a capture
    // sink, and a call smuggling a MAC, a credential, an opaque identifier and
    // a reserved-key field the caller never redacted.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    const aa::core::Logger logger{safe};
    logger.log(aa::core::LogLevel::info, "peer AA:BB:CC:DD:EE:FF linked",
               {{"password", "hunter2-secret"},
                {"reason", "link lost"},
                {"opaque_id", "8F3A22B19C0D77EE"},
                {"session", "9"}});
    // When/Then: only redacted content crosses; reserved keys never arrive.
    ASSERT_EQ(capture.events.size(), std::size_t{1});
    const CaptureLogSink::Captured& event = capture.events[0];
    EXPECT_EQ(event.message, "[redacted]");
    EXPECT_EQ(event.message.find("AA:BB:CC:DD:EE:FF"), std::string::npos);
    ASSERT_EQ(event.fields.size(), std::size_t{2});
    EXPECT_EQ(event.fields[0].key, "password");
    EXPECT_EQ(event.fields[0].value, "[redacted]");
    EXPECT_EQ(event.fields[1].key, "reason");
    EXPECT_EQ(event.fields[1].value, "link lost");
    const std::string joined = event.message + event.fields[0].value +
                               event.fields[1].value;
    EXPECT_EQ(joined.find("hunter2-secret"), std::string::npos);
    EXPECT_EQ(joined.find("8F3A22B19C0D77EE"), std::string::npos);
}

TEST(DiagnosticsSafeLogSink, RedactsArbitraryMessagesAndKeepsLevelFilteringUpstream) {
    // Given: a secret key=value shape inside the message and a below-minimum
    // event behind the same adapter.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    const aa::core::Logger logger{safe, aa::core::LogLevel::error};
    logger.log(aa::core::LogLevel::warning, "retry psk=s3cr3t");
    logger.log(aa::core::LogLevel::error, "retry psk=s3cr3t");
    // When/Then: the arbitrary message is replaced before reaching the backend and
    // the level filter drops the warning before the adapter sees it.
    ASSERT_EQ(capture.events.size(), std::size_t{1});
    EXPECT_EQ(capture.events[0].message.find("s3cr3t"), std::string::npos);
    EXPECT_TRUE(contains(capture.events[0].message, "[redacted]"));
}

} // namespace
