// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/diagnostics/SafeLogSink.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
namespace diag = aa::diagnostics;

struct CaptureLogSink final : aa::core::LogSink {
    std::string message;
    std::vector<aa::core::LogField> fields;
    void write(const aa::core::LogEvent& event) override {
        message = event.message;
        fields = event.fields;
    }
};

TEST(DiagnosticsEmissionSchema, HidesOpaqueMessageWhenLoggedThroughAdapter) {
    // Given: a secret without a pattern the scrubber recognizes.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    const aa::core::Logger logger{safe};
    // When: an arbitrary message crosses the actual core log adapter.
    logger.log(aa::core::LogLevel::info, "8F3A22B19C0D77EE");
    // Then: only a fixed replacement reaches the backend.
    EXPECT_EQ(capture.message, "[redacted]");
}

TEST(DiagnosticsEmissionSchema, HidesOpaqueStringsWhenOnKnownSerializerKeys) {
    // Given: permitted names do not make their values telemetry.
    diag::Event event{};
    for (const char* key : {"reason", "status", "version"}) {
        event.fields.push_back(diag::EventField::text(key, "8F3A22B19C0D77EE"));
    }
    // When: the serializer enforces the value schema.
    const std::string out = diag::to_json(event);
    // Then: none of the opaque strings survives.
    EXPECT_EQ(out.find("8F3A22B19C0D77EE"), std::string::npos);
}

TEST(DiagnosticsEmissionSchema, HidesOpaqueStringsWhenOnKnownLogKeys) {
    // Given: the core API carries all values as strings.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    // When: permitted keys carry opaque secrets, including a numeric-looking string.
    safe.write({aa::core::LogLevel::info, "reconnect",
                {{"reason", "8F3A22B19C0D77EE"}, {"status", "424242"},
                 {"version", "8F3A22B19C0D77EE"}}});
    // Then: string enums and versions fail closed regardless of spelling.
    ASSERT_EQ(capture.fields.size(), std::size_t{3});
    for (const auto& field : capture.fields) {
        EXPECT_EQ(field.value, "[redacted]");
    }
}

TEST(DiagnosticsEmissionSchema, DropsOpaqueKeyWhenSerialized) {
    // Given: even the field name is an opaque secret.
    diag::Event event{};
    event.fields.push_back(diag::EventField::identifier("8F3A22B19C0D77EE", "another-secret"));
    event.fields.push_back(diag::EventField::text("password8F3A22B19C0D77EE", "secret"));
    event.fields.push_back(diag::EventField::count("f_r_a_m_e_s", 30));
    // When: the boundary serializes unknown and disguised keys.
    const std::string out = diag::to_json(event);
    // Then: no dynamic key spelling can leak.
    EXPECT_EQ(out, "{\"kind\":\"session_state_changed\",\"session\":0,\"mono_ns\":0}");
}

TEST(DiagnosticsEmissionSchema, DropsOpaqueKeyWhenLogged) {
    // Given: unknown and sensitive-looking names can contain secrets.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    // When: those names cross the adapter.
    safe.write({aa::core::LogLevel::info, "helper_call",
                {{"8F3A22B19C0D77EE", "value"}, {"password8F3A22B19C0D77EE", "value"}}});
    // Then: unknown names are absent, not merely value-redacted.
    EXPECT_TRUE(capture.fields.empty());
}

TEST(DiagnosticsEmissionSchema, HidesNumericSensitiveVariantsWhenOnSafeKeys) {
    // Given: declared sensitivity must dominate even a matching safe numeric key.
    diag::Event event{};
    event.fields = {{"frames", diag::Sensitivity::credential, std::int64_t{424242}},
                    {"fps", diag::Sensitivity::location, 37.7749},
                    {"count", diag::Sensitivity::identifier, std::int64_t{123456789012345}},
                    {"ok", diag::Sensitivity::credential, true}};
    // When: all variants cross emission.
    const std::string out = diag::to_json(event);
    // Then: neither raw numbers nor booleans survive.
    EXPECT_EQ(out.find("424242"), std::string::npos);
    EXPECT_EQ(out.find("37.7749"), std::string::npos);
    EXPECT_EQ(out.find("123456789012345"), std::string::npos);
    EXPECT_EQ(out.find("true"), std::string::npos);
    EXPECT_NE(out.find("\"count\":\"id-"), std::string::npos);
}

TEST(DiagnosticsEmissionSchema, HidesInvalidValuesWhenTypesOrVersionFormatsMismatch) {
    // Given: strings cannot masquerade as counts; versions cannot have arbitrary suffixes.
    diag::Event event{};
    event.fields = {diag::EventField::text("frames", "424242"),
                    diag::EventField::count("reason", 424242),
                    diag::EventField::text("target_version", "jetson-r39.2.1-8F3A22B19C0D77EE"),
                    diag::EventField::text("phone_version", "Android 16 AA 17.7.663654 secret"),
                    diag::EventField::text("version", "999999999999999999.1"),
                    diag::EventField::count("count", -1)};
    // When: the typed schema validates the values.
    const std::string out = diag::to_json(event);
    // Then: all invalid values are redacted.
    EXPECT_EQ(out.find("424242"), std::string::npos);
    EXPECT_EQ(out.find("8F3A22B19C0D77EE"), std::string::npos);
    EXPECT_EQ(out.find("secret"), std::string::npos);
    EXPECT_EQ(out.find("999999999999999999"), std::string::npos);
    EXPECT_EQ(out.find(":-1"), std::string::npos);
}

TEST(DiagnosticsEmissionSchema, PreservesTelemetryWhenLoggedAsValidatedStrings) {
    // Given: real schema data carried by the string-only core API.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    const std::vector<aa::core::LogField> fields = {
        {"from_state", "discovering"}, {"to_state", "connecting"},
        {"target_version", "jetson-r39.2.1"}, {"phone_version", "Android 16 AA 17.7.663654"},
        {"reason", "link lost"}, {"status", "ok"}, {"frames", "18000"},
        {"fps", "30.5"}, {"p95_latency_ms", "33.5"}, {"ok", "true"}};
    // When: a stable event name and validated telemetry cross the adapter.
    safe.write({aa::core::LogLevel::info, "session_state_changed", fields});
    // Then: legitimate telemetry remains useful and unchanged.
    EXPECT_EQ(capture.message, "session_state_changed");
    ASSERT_EQ(capture.fields.size(), fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        EXPECT_EQ(capture.fields[i].key, fields[i].key);
        EXPECT_EQ(capture.fields[i].value, fields[i].value);
    }
}

TEST(DiagnosticsEmissionSchema, HidesMalformedScalarsWhenLogged) {
    // Given: string-only scalars must parse completely and match their domain.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    // When: invalid, non-finite and wrong-type values cross emission.
    safe.write({aa::core::LogLevel::info, "transport_stats",
                {{"frames", "42 secret"}, {"frames", "1.5"}, {"frames", "-1"},
                 {"fps", "nan"}, {"fps", "inf"}, {"fps", "-1"}, {"fps", ""},
                 {"ok", "true secret"}, {"ok", "424242"}}});
    // Then: none reaches the backend as telemetry.
    ASSERT_EQ(capture.fields.size(), std::size_t{9});
    for (const auto& field : capture.fields) EXPECT_EQ(field.value, "[redacted]");
}

TEST(DiagnosticsEmissionSchema, KeepsEveryStableMessageWhenInEventVocabulary) {
    // Given: the stable messages are the existing typed event vocabulary.
    CaptureLogSink capture;
    diag::SafeLogSink safe{capture};
    for (const diag::EventKind kind : {diag::EventKind::session_state_changed,
                                     diag::EventKind::transport_stats, diag::EventKind::decode_stats,
                                     diag::EventKind::reconnect, diag::EventKind::pairing_requested,
                                     diag::EventKind::pairing_decided,
                                     diag::EventKind::capability_negotiated, diag::EventKind::helper_call}) {
        // When: a valid event name crosses emission.
        safe.write({aa::core::LogLevel::info, diag::to_string(kind), {}});
        // Then: the backend retains the event identity.
        EXPECT_EQ(capture.message, diag::to_string(kind));
    }
}

TEST(DiagnosticsEmissionSchema, ScrubsKnownShapesWithoutClaimingOpaqueSecretDetection) {
    // Given: both recognizable privacy patterns and an opaque string.
    const std::string text = "AA:BB:CC:DD:EE:FF 37.7749,-122.4194 password=hunter2-secret";
    // When: the independent pattern layer runs.
    const std::string out = diag::scrub_text(text);
    // Then: known patterns disappear; opaque input demonstrates this layer's limit.
    EXPECT_EQ(out.find("AA:BB:CC:DD:EE:FF"), std::string::npos);
    EXPECT_EQ(out.find("37.7749"), std::string::npos);
    EXPECT_EQ(out.find("-122.4194"), std::string::npos);
    EXPECT_EQ(out.find("hunter2-secret"), std::string::npos);
    EXPECT_EQ(diag::scrub_text("8F3A22B19C0D77EE"), "8F3A22B19C0D77EE");
}
} // namespace
