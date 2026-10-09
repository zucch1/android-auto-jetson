// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only probe: prints one redacted JSON object per line for a set of
// synthetic diagnostics fixtures (raw MAC, location, credentials, phone id,
// special characters). tests/diagnostics/test_json.py parses each line with a
// real JSON parser to prove the serializer emits valid JSON and never leaks the
// raw values. This is test infrastructure, not a production diagnostics tool.
#include <aa/diagnostics/Diagnostics.hpp>

#include <iostream>
#include <string>

namespace diag = aa::diagnostics;

namespace {

void emit(const diag::Event& event) { std::cout << diag::to_json(event) << "\n"; }

} // namespace

int main() {
    // Fixture 1: structured schema (session/kind/mono_ns) + versions + typed
    // identifier carrying a raw Bluetooth MAC.
    diag::Event schema{};
    schema.session = aa::core::SessionId{42};
    schema.kind = diag::EventKind::session_state_changed;
    schema.monotonic_time = aa::core::Nanoseconds{123456789};
    schema.fields.push_back(diag::EventField::text("from_state", "discovering"));
    schema.fields.push_back(diag::EventField::text("to_state", "connecting"));
    schema.fields.push_back(diag::EventField::text("target_version", "jetson-r39.2.1"));
    schema.fields.push_back(diag::EventField::text("phone_version", "Android 16 AA 17.7.663654"));
    schema.fields.push_back(diag::EventField::identifier("phone_mac", "AA:BB:CC:DD:EE:FF"));
    emit(schema);

    // Fixture 2: secrets smuggled through an unknown/dynamic field plus typed
    // credential and key=value credential shapes.
    diag::Event leak{};
    leak.kind = diag::EventKind::helper_call;
    leak.fields.push_back(diag::EventField::text(
        "x7", "junk psk=s3cr3t Authorization: Bearer abc.def imei=123456789012345"));
    leak.fields.push_back(diag::EventField::credential("psk", "hunter2-secret"));
    leak.fields.push_back(diag::EventField::text("diag", "password=hunter2-secret token=tok-999"));
    emit(leak);

    // Fixture 3: typed location plus a raw coordinate pair embedded in text.
    diag::Event loc{};
    loc.kind = diag::EventKind::transport_stats;
    loc.fields.push_back(diag::EventField::location("gps", "37.7749,-122.4194"));
    loc.fields.push_back(diag::EventField::text("where", "near 37.7749,-122.4194 now"));
    emit(loc);

    // Fixture 4: freeform special characters fail closed; typed stats survive.
    std::string tricky = "he said \"hi\" \\ then\nnew\ttab";
    tricky.push_back('\x01');
    diag::Event esc{};
    esc.kind = diag::EventKind::decode_stats;
    esc.fields.push_back(diag::EventField::text("reason", tricky));
    esc.fields.push_back(diag::EventField::count("frames", 18000));
    esc.fields.push_back(diag::EventField::real("p95_latency_ms", 33.5));
    esc.fields.push_back(diag::EventField::flag("dropped", false));
    emit(esc);

    // Fixture 5 (task 19 correction repro): reserved envelope override
    // attempts, an unlabelled secret on a sensitive key, numeric locations, an
    // unknown opaque identifier and invalid UTF-8 in a safe-schema value.
    diag::Event gap{};
    gap.session = aa::core::SessionId{9};
    gap.kind = diag::EventKind::reconnect;
    gap.monotonic_time = aa::core::Nanoseconds{7};
    gap.fields.push_back(diag::EventField::text("kind", "evil"));
    gap.fields.push_back(diag::EventField::text("session", "999"));
    gap.fields.push_back(diag::EventField::text("mono_ns", "1"));
    gap.fields.push_back(diag::EventField::text("password", "unlabelled-secret"));
    gap.fields.push_back(diag::EventField::real("lat", 37.7749));
    gap.fields.push_back(
        diag::EventField{"gps", diag::Sensitivity::location, 37.7749});
    gap.fields.push_back(diag::EventField::text("opaque_id", "8F3A22B19C0D77EE"));
    std::string bad = "tail-";
    bad.push_back('\xFF');
    bad += "-head";
    gap.fields.push_back(diag::EventField::text("reason", bad));
    emit(gap);

    // Fixture 6: opaque safe-field strings, a secret key, and sensitive numeric telemetry.
    diag::Event opaque{};
    opaque.kind = diag::EventKind::pairing_decided;
    opaque.fields = {diag::EventField::text("reason", "8F3A22B19C0D77EE"),
                     diag::EventField::text("status", "8F3A22B19C0D77EE"),
                     diag::EventField::text("version", "8F3A22B19C0D77EE"),
                     diag::EventField::text("8F3A22B19C0D77EE", "opaque-value"),
                     {"frames", diag::Sensitivity::credential, std::int64_t{424242}}};
    emit(opaque);

    return 0;
}
