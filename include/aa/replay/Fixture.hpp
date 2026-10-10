// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/Audio.hpp>
#include <aa/core/Error.hpp>
#include <aa/core/Time.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/Protocol.hpp>
#include <aa/session/Events.hpp>
#include <aa/session/Session.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aa::replay {

// Versioned capture/replay fixture schema (task 20). One file is
//   {"schema":"aa-replay-fixture-v1","sha256":"<hex>","body":<canonical>}
// where <canonical> is the exact canonical body byte string (fixed key order,
// no whitespace, hex payloads) and sha256 is the lowercase hex SHA-256 of
//   "aa-replay-fixture-v1\n" || <canonical>
// computed over those fixed canonical bytes - never over diagnostics salted
// pseudonyms. The body keys are exactly, in order: provenance, metadata,
// services, records, expect.
//
// PROVENANCE CONTRACT: synthetic-only. A fixture records caller-supplied
// synthetic payloads under an explicit opaque policy marker ("opaque":
// "synthetic") on every byte blob. Opaque message/media bytes cannot be
// privacy-scrubbed by regex or base64 and none is claimed: raw local capture
// is out of scope (no raw-capture/export split exists), and public fixtures
// must carry only synthetic provenance and known synthetic payloads. The
// metadata section is the only privacy-checked text: it is exported through
// the task-19 diagnostics boundary (to_json) with fixed redaction.
//
// A diagnostic privacy gate is not an integrity signature and proves no media
// decode or hardware qualification.

inline constexpr std::string_view kSchemaName = "aa-replay-fixture-v1";
inline constexpr std::string_view kSyntheticProvenance = "synthetic";
inline constexpr std::string_view kOpaquePolicy = "synthetic";

// Byte/count/time bounds. Real module bounds are reused where they exist
// (wire frame, access unit, discovered services); the rest cap fixture growth
// so no record path can force an unbounded allocation.
inline constexpr std::size_t kMaxRecords = 1024;
inline constexpr std::size_t kMaxServices = 64;
inline constexpr std::size_t kMaxAudioPayloadBytes = std::size_t{64} * 1024U;
inline constexpr std::size_t kMaxInputPayloadBytes = std::size_t{16} * 1024U;
inline constexpr std::size_t kMaxTotalPayloadBytes = std::size_t{8} * 1024U * 1024U;
inline constexpr std::int64_t kMaxTraceNs = 3'600'000'000'000LL; // 1 h monotonic span
inline constexpr std::size_t kMaxFixtureBytes = std::size_t{1024} * 1024U;
inline constexpr std::size_t kMaxMetadataFields = 32;
inline constexpr std::size_t kMaxMetadataTextBytes = 4096;
inline constexpr std::size_t kMaxPhoneKeyBytes = 128;

// Running admission counters for the record-count, trace-duration and total
// payload budgets. A rejected record mutates none of them.
struct AdmissionBudget final {
    std::size_t records{};
    std::size_t payload_bytes{};
    std::int64_t first_t{};
    std::int64_t last_t{};
    bool have_t{};
};

// Only synthetic capture is representable.
enum class Provenance { synthetic };

enum class RecordKind { session, transport, video, audio, input };

[[nodiscard]] constexpr std::string_view to_string(RecordKind kind) noexcept {
    switch (kind) {
    case RecordKind::session: return "session";
    case RecordKind::transport: return "transport";
    case RecordKind::video: return "video";
    case RecordKind::audio: return "audio";
    case RecordKind::input: return "input";
    }
    return "unknown";
}

enum class Direction { inbound, outbound };

[[nodiscard]] constexpr std::string_view to_string(Direction dir) noexcept {
    switch (dir) {
    case Direction::inbound: return "in";
    case Direction::outbound: return "out";
    }
    return "unknown";
}

// Typed lifecycle script step (task-17 Lifecycle seam). `phone_key` is a
// synthetic trust-store key for the identity ops; `fail_code` for fail.
enum class SessionOp {
    event,
    phone_discovered,
    authorize_pairing,
    confirm_pairing,
    fail,
    request_stop,
    tick,
    attach_fresh,
    reset_channels,
};

struct SessionRecord final {
    SessionOp op{SessionOp::tick};
    session::Event event{session::Event::reset};
    std::string phone_key{};
    ErrorCode fail_code{ErrorCode::internal};

    [[nodiscard]] friend bool operator==(const SessionRecord&, const SessionRecord&) = default;
};

struct TransportRecord final {
    Direction dir{Direction::inbound};
    std::vector<std::byte> frame{};

    [[nodiscard]] friend bool operator==(const TransportRecord&, const TransportRecord&) = default;
};

struct VideoRecord final {
    std::uint64_t frame{};
    std::uint64_t sequence{};
    core::Nanoseconds timestamp{};
    bool idr{};
    std::vector<std::byte> payload{};

    [[nodiscard]] friend bool operator==(const VideoRecord&, const VideoRecord&) = default;
};

struct AudioRecord final {
    audio::Role role{audio::Role::media};
    audio::AudioFormat format{};
    core::Nanoseconds timestamp{};
    std::vector<std::byte> pcm{};

    [[nodiscard]] friend bool operator==(const AudioRecord&, const AudioRecord&) = default;
};

struct InputRecord final {
    std::uint64_t sequence{};
    core::Nanoseconds timestamp{};
    std::vector<std::byte> payload{};

    [[nodiscard]] friend bool operator==(const InputRecord&, const InputRecord&) = default;
};

using RecordBody = std::variant<SessionRecord, TransportRecord, VideoRecord, AudioRecord,
                                InputRecord>;

struct Record final {
    RecordKind kind{RecordKind::session};
    core::Nanoseconds t{};
    RecordBody body{SessionRecord{}};

    [[nodiscard]] friend bool operator==(const Record&, const Record&) = default;
};

// Advertised service snapshot (task-18 SupportConfiguration input).
struct ServiceEntry final {
    protocol::ServiceKey id{};
    protocol::ChannelRole role{protocol::ChannelRole::video};
    protocol::ServiceConfiguration configuration{};

    [[nodiscard]] friend bool operator==(const ServiceEntry&, const ServiceEntry&) = default;
};

// Explicit expected output trace. `states` are to_string(session::State) after
// each session record; deliveries and audio are the exact sink outcomes.
struct DeliveryOutcome final {
    protocol::ChannelRole role{protocol::ChannelRole::video};
    std::uint64_t sequence{};
    core::Nanoseconds timestamp{};
    std::vector<std::byte> payload{};

    [[nodiscard]] friend bool operator==(const DeliveryOutcome&, const DeliveryOutcome&) = default;
};

struct AudioOutcome final {
    audio::Role role{audio::Role::media};
    audio::AudioFormat format{};
    core::Nanoseconds timestamp{};
    std::vector<std::byte> pcm{};

    [[nodiscard]] friend bool operator==(const AudioOutcome&, const AudioOutcome&) = default;
};

struct Expectation final {
    std::vector<std::string> states{};
    std::vector<DeliveryOutcome> deliveries{};
    std::vector<AudioOutcome> audio{};
    std::int64_t sends{};

    [[nodiscard]] friend bool operator==(const Expectation&, const Expectation&) = default;
};

struct Fixture final {
    Provenance provenance{Provenance::synthetic};
    // Single-line diagnostics to_json output (post fixed-redaction). This is
    // the only text that crosses the task-19 privacy boundary.
    std::string metadata{};
    std::vector<ServiceEntry> services{};
    std::vector<Record> records{};
    Expectation expect{};
};

// Exported artifact: the exact fixture file bytes plus its content hash.
struct ExportedFixture final {
    std::vector<std::byte> file_bytes{};
    std::string sha256_hex{};
};

} // namespace aa::replay
