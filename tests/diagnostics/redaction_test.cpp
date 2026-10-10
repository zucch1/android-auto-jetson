// SPDX-License-Identifier: GPL-3.0-or-later
// Task 19 diagnostics privacy tests: the serializer/emission boundary redacts
// raw MAC / phone identifiers / locations / credentials even when the caller
// passes them raw, unknown dynamic fields cannot leak embedded secrets, and the
// output carries the structured schema (session, kind, monotonic time) with
// correct JSON escaping. Fields-remain-valid-JSON is proven authoritatively in
// tests/diagnostics/test_json.py via a real JSON parser.
#include <aa/diagnostics/Diagnostics.hpp>

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace diag = aa::diagnostics;

struct CaptureSink final : diag::DiagnosticSink {
    std::vector<std::string> jsons;
    void emit_json(std::string_view redacted_json) override {
        jsons.emplace_back(redacted_json);
    }
};

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

TEST(DiagnosticsRedaction, RedactIdentifierHidesRawBehindStablePseudonym) {
    // Given: a raw identifier that must never reach logs or events.
    const std::string raw{"AA:BB:CC:DD:EE:FF"};
    // When: the pseudonymizer runs twice within one process.
    const auto first = diag::redact_identifier(raw);
    const auto second = diag::redact_identifier(raw);
    // Then: the pseudonym is stable, namespaced and never contains the raw value.
    EXPECT_EQ(first, second);
    EXPECT_TRUE(first.rfind("id-", 0) == 0);
    EXPECT_EQ(first.find(raw), std::string::npos);
    EXPECT_TRUE(diag::redact_identifier(std::string_view{}).empty());
}

TEST(DiagnosticsRedaction, SerializerRedactsMacInTypedIdentifierField) {
    // Given: an event carrying a raw Bluetooth MAC as a typed identifier.
    diag::Event event{};
    event.session = aa::core::SessionId{7};
    event.kind = diag::EventKind::session_state_changed;
    event.fields.push_back(diag::EventField::identifier("phone_mac", "AA:BB:CC:DD:EE:FF"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: the raw MAC cannot escape; only a stable pseudonym remains.
    EXPECT_EQ(out.find("AA:BB:CC:DD:EE:FF"), std::string::npos);
    EXPECT_TRUE(contains(out, "\"phone_mac\":\"id-"));
}

TEST(DiagnosticsRedaction, UnknownNamesDropAndKnownFreeformValuesRedact) {
    // Given: a MAC in an unknown field and in a known enum field.
    diag::Event event{};
    event.kind = diag::EventKind::helper_call;
    event.fields.push_back(diag::EventField::text("note", "peer device AA:BB:CC:DD:EE:FF seen"));
    event.fields.push_back(
        diag::EventField::text("reason", "peer device AA:BB:CC:DD:EE:FF seen"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: neither arbitrary names nor arbitrary enum strings survive.
    EXPECT_EQ(out.find("AA:BB:CC:DD:EE:FF"), std::string::npos);
    EXPECT_EQ(out.find("\"note\""), std::string::npos);
    EXPECT_TRUE(contains(out, "\"reason\":\"[redacted]\""));
}

TEST(DiagnosticsRedaction, SerializerRedactsTypedLocationAndCoordinateText) {
    // Given: a typed location field plus a coordinate pair embedded in free text.
    diag::Event event{};
    event.kind = diag::EventKind::transport_stats;
    event.fields.push_back(diag::EventField::location("gps", "37.7749,-122.4194"));
    event.fields.push_back(diag::EventField::text("where", "near 37.7749,-122.4194 now"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: neither the typed location nor the raw coordinates survive.
    EXPECT_EQ(out.find("37.7749"), std::string::npos);
    EXPECT_EQ(out.find("-122.4194"), std::string::npos);
    EXPECT_TRUE(contains(out, "[redacted]"));
}

TEST(DiagnosticsRedaction, SerializerRedactsTypedCredentialAndKeyValues) {
    // Given: a typed credential plus credential shapes in an unknown field.
    diag::Event event{};
    event.kind = diag::EventKind::pairing_requested;
    event.fields.push_back(diag::EventField::credential("psk", "hunter2-secret"));
    event.fields.push_back(diag::EventField::text("diag", "password=hunter2-secret token=tok-999"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: the raw secrets cannot escape via the typed path or the scrub.
    EXPECT_EQ(out.find("hunter2-secret"), std::string::npos);
    EXPECT_EQ(out.find("tok-999"), std::string::npos);
    EXPECT_TRUE(contains(out, "[redacted]"));
}

TEST(DiagnosticsRedaction, UnknownDynamicFieldCannotLeakCredentialOrPhoneId) {
    // Given: an unrecognized dynamic field whose value smuggles a secret, an
    // Authorization header and a phone identifier.
    diag::Event event{};
    event.kind = diag::EventKind::helper_call;
    event.fields.push_back(diag::EventField::text(
        "x7", "junk psk=s3cr3t Authorization: Bearer abc.def imei=123456789012345"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: every embedded secret/identifier is redacted despite the unknown key.
    EXPECT_EQ(out.find("s3cr3t"), std::string::npos);
    EXPECT_EQ(out.find("abc.def"), std::string::npos);
    EXPECT_EQ(out.find("123456789012345"), std::string::npos);
    EXPECT_EQ(out.find("\"x7\""), std::string::npos);
}

TEST(DiagnosticsRedaction, KeepsStructuredSchemaAndSafeVersionFields) {
    // Given: a session state transition with reconnect reason and target/phone
    // versions (safe, non-identifier metadata that must survive).
    diag::Event event{};
    event.session = aa::core::SessionId{42};
    event.kind = diag::EventKind::session_state_changed;
    event.monotonic_time = aa::core::Nanoseconds{123456789};
    event.fields.push_back(diag::EventField::text("from_state", "discovering"));
    event.fields.push_back(diag::EventField::text("to_state", "connecting"));
    event.fields.push_back(diag::EventField::text("target_version", "jetson-r39.2.1"));
    event.fields.push_back(diag::EventField::text("phone_version", "Android 16 AA 17.7.663654"));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: the structured schema and version strings are intact and escaped.
    EXPECT_TRUE(contains(out, "\"kind\":\"session_state_changed\""));
    EXPECT_TRUE(contains(out, "\"session\":42"));
    EXPECT_TRUE(contains(out, "\"mono_ns\":123456789"));
    EXPECT_TRUE(contains(out, "\"from_state\":\"discovering\""));
    EXPECT_TRUE(contains(out, "\"to_state\":\"connecting\""));
    EXPECT_TRUE(contains(out, "\"target_version\":\"jetson-r39.2.1\""));
    EXPECT_TRUE(contains(out, "Android 16 AA 17.7.663654"));
}

TEST(DiagnosticsRedaction, TypedNumericAndBooleanFieldsAreJsonScalars) {
    // Given: transport/decode stats expressed as typed count/real/flag fields.
    diag::Event event{};
    event.kind = diag::EventKind::decode_stats;
    event.fields.push_back(diag::EventField::count("frames", 18000));
    event.fields.push_back(diag::EventField::real("fps", 30.5));
    event.fields.push_back(diag::EventField::real("p95_latency_ms", 33.5));
    event.fields.push_back(diag::EventField::flag("dropped", false));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: numbers and booleans are bare JSON scalars, not quoted strings.
    EXPECT_TRUE(contains(out, "\"frames\":18000"));
    EXPECT_TRUE(contains(out, "\"fps\":30.5"));
    EXPECT_TRUE(contains(out, "\"p95_latency_ms\":33.5"));
    EXPECT_TRUE(contains(out, "\"dropped\":false"));
}

TEST(DiagnosticsRedaction, RejectsSpecialCharactersAtEmissionButScrubPreservesBenignText) {
    // Given: freeform text with quote, backslash, newline, tab and control bytes.
    std::string tricky = "he said \"hi\" \\ then\nnew\ttab";
    tricky.push_back('\x01');
    diag::Event event{};
    event.kind = diag::EventKind::helper_call;
    event.fields.push_back(diag::EventField::text("reason", tricky));
    // When: the boundary serializes it.
    const std::string out = diag::to_json(event);
    // Then: the pattern API preserves benign text, but emission rejects it.
    EXPECT_EQ(diag::scrub_text(tricky), tricky);
    EXPECT_FALSE(contains(out, "he said \"hi\""));
    EXPECT_TRUE(contains(out, "\"reason\":\"[redacted]\""));
    for (const char c : out) {
        EXPECT_GE(static_cast<unsigned char>(c), 0x20U);
    }
}

TEST(DiagnosticsRedaction, EmitEnforcesRedactionAtTheSinkBoundary) {
    // Given: a capturing sink behind Diagnostics and an event carrying a raw
    // credential and MAC the caller never redacted.
    CaptureSink sink;
    const diag::Diagnostics diagnostics{sink};
    diag::Event event{};
    event.kind = diag::EventKind::reconnect;
    event.fields.push_back(diag::EventField::text("reason", "link lost"));
    event.fields.push_back(diag::EventField::text("leak", "password=hunter2-secret"));
    event.fields.push_back(diag::EventField::text("peer", "AA:BB:CC:DD:EE:FF"));
    // When: the event is emitted.
    diagnostics.emit(event);
    // Then: the sink only ever observes redacted JSON; the raw secrets are gone.
    ASSERT_EQ(sink.jsons.size(), std::size_t{1});
    EXPECT_EQ(sink.jsons[0].find("hunter2-secret"), std::string::npos);
    EXPECT_EQ(sink.jsons[0].find("AA:BB:CC:DD:EE:FF"), std::string::npos);
    EXPECT_TRUE(contains(sink.jsons[0], "\"reason\":\"link lost\""));
}

TEST(DiagnosticsRedaction, ScrubTextIsIdempotentOnAlreadySafeText) {
    // Given: text with no sensitive shape.
    const std::string safe = "session active fps=30.0 ok";
    // When/Then: scrubbing preserves benign content and is stable.
    EXPECT_EQ(diag::scrub_text(safe), safe);
    const std::string once = diag::scrub_text("token=abc123");
    EXPECT_FALSE(contains(once, "abc123"));
    EXPECT_EQ(diag::scrub_text(once), once);
}

} // namespace
