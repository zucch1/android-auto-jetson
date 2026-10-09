// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/core/Time.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aa::diagnostics {

// Structured diagnostics and the privacy redaction boundary (task 19).
//
// PRIVACY CONTRACT: to_json()/Diagnostics::emit() enforce an explicit schema,
// not a caller promise to pre-redact. Only exact reviewed key names emit;
// unknown names (even sensitive-looking names) and reserved envelope names
// are dropped because a name can itself contain a secret. On known names,
// identifiers pseudonymize and credentials/locations redact for ALL variants.
// Plain sensitive fields redact. Plain telemetry must match the key's type:
// finite enum strings, numeric versions (2-4 decimal components, 1-6 digits
// each, optionally "jetson-r" or "Android <1-2 digits> AA " prefixes),
// nonnegative counts/finite rates or boolean flags. Arbitrary freeform strings
// redact, including on reason/status/version. The exact tables and validators
// live in src/diagnostics/{Policy,ValuePolicy}.cpp.
//
// Typed session/time/numeric telemetry are authorized data, not covert-channel
// detectors: a secret intentionally encoded as a valid count, enum or version
// is indistinguishable from telemetry. Producers must use these for their
// declared purpose and label sensitive values. No arbitrary-number secret
// recognition or cryptographic identifier-hiding guarantee is claimed.
//
// Ownership/threading: the sink is owned by the process entry point and must
// tolerate emit_json() from any thread (session, transport, audio).

enum class EventKind {
    session_state_changed,
    transport_stats,
    decode_stats,
    reconnect,
    pairing_requested,
    pairing_decided,
    capability_negotiated,
    helper_call,
};

[[nodiscard]] std::string_view to_string(EventKind kind) noexcept;

// How the serializer treats a dynamic field value. Declared sensitivity can
// only strengthen enforcement (pseudonym/redact); it can never authorize a raw
// value: a `plain` field survives only with a known name and validated value.
enum class Sensitivity {
    plain,       // ordinary telemetry; must match the safe schema
    identifier,  // phone/MAC/serial id -> stable per-process pseudonym
    location,    // coordinates / place -> dropped
    credential,  // secret / token / psk -> dropped
};

// A typed field value: exactly one of text, integer, real or boolean. The
// variant makes a value that is both a number and a string unrepresentable.
using FieldValue = std::variant<std::string, std::int64_t, double, bool>;

struct EventField final {
    std::string key;
    Sensitivity sensitivity{Sensitivity::plain};
    FieldValue value{};

    // Typed constructors (the parsing/typing boundary for callers).
    [[nodiscard]] static EventField text(std::string key, std::string value);
    [[nodiscard]] static EventField identifier(std::string key, std::string raw);
    [[nodiscard]] static EventField location(std::string key, std::string raw);
    [[nodiscard]] static EventField credential(std::string key, std::string raw);
    [[nodiscard]] static EventField count(std::string key, std::int64_t value);
    [[nodiscard]] static EventField real(std::string key, double value);
    [[nodiscard]] static EventField flag(std::string key, bool value);
};

struct Event final {
    core::SessionId session{};
    EventKind kind{};
    core::Nanoseconds monotonic_time{};
    // Typed dynamic fields (state transitions, transport/decode stats,
    // reconnect reason, target/phone versions). Values may be raw; the
    // serializer enforces the safe telemetry schema. Known fields are emitted
    // in insertion order; unknown names are dropped.
    std::vector<EventField> fields{};
};

// THE PRIVACY BOUNDARY. Serializes one event to a single-line JSON object:
//   {"kind":"<kind>","session":<uint>,"mono_ns":<int>,"<key>":<value>,...}
// Every field crosses the exact-name, sensitivity and typed-value policy.
// Unknown/reserved names drop; invalid values redact. Surviving strings are
// pattern-scrubbed and JSON escaped. The result is valid single-line JSON.
[[nodiscard]] std::string to_json(const Event& event);

// Pattern scrub and UTF-8 repair for one piece of free text: replaces embedded
// MAC shapes with a pseudonym, sensitive key=value / coordinate shapes with
// "[redacted]", and invalid UTF-8 bytes with U+FFFD. This is only a pattern
// layer, NOT a recognizer for arbitrary opaque secrets or numeric covert data.
[[nodiscard]] std::string scrub_text(std::string_view text);

// Redaction boundary for one identifier. Returns a stable per-process pseudonym
// ("id-<hex>") for a raw identifier; the same raw value maps to the same
// pseudonym within one process and the output never contains the input. Empty
// input stays empty. Salt-seeded FNV supports only in-run correlation. It is
// noncryptographic: no offline-guessing resistance, cross-run unlinkability,
// irreversibility or authenticity guarantee.
[[nodiscard]] std::string redact_identifier(std::string_view raw);

// Diagnostics passes only schema-enforced JSON to this sink. Direct calls to
// emit_json() are not a privacy boundary. Implementations must tolerate calls
// from any thread.
class DiagnosticSink {
public:
    virtual ~DiagnosticSink() = default;
    DiagnosticSink() = default;
    DiagnosticSink(const DiagnosticSink&) = delete;
    DiagnosticSink& operator=(const DiagnosticSink&) = delete;
    DiagnosticSink(DiagnosticSink&&) = delete;
    DiagnosticSink& operator=(DiagnosticSink&&) = delete;

    virtual void emit_json(std::string_view redacted_json) = 0;
};

// Non-owning facade. emit() routes the event through the serializer so the sink
// only ever sees redacted JSON; redaction is enforced here, not promised by the
// caller who built the event.
class Diagnostics final {
public:
    explicit Diagnostics(DiagnosticSink& sink) noexcept : sink_(&sink) {}

    void emit(const Event& event) const { sink_->emit_json(to_json(event)); }

private:
    DiagnosticSink* sink_;
};

} // namespace aa::diagnostics
