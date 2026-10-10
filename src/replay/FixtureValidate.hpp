// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/replay/Fixture.hpp>

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace aa::replay::detail {

// Shared schema vocabulary and validation used by both export and load, so
// the two gates cannot diverge. Bounds, ID consistency, state-trace legality
// (real session transition table) and metadata counters all land here.

[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);
[[nodiscard]] core::Result<std::vector<std::byte>> from_hex(std::string_view text);

// Shared admission rule for record count, trace duration and total payload
// bytes, committed atomically so a rejected record mutates nothing. Used by
// both the recorder and the fixture gate.
[[nodiscard]] core::Result<void> admit_record(AdmissionBudget& budget, core::Nanoseconds t,
                                              std::size_t payload_bytes);

// Typed boundary rules shared by records and expectations (accepted public
// contract: audio formats follow protocol::valid_audio_profile, media times
// are nonnegative monotonic nanoseconds, phone keys are synthetic and short).
// Typed enum/ID vocabulary: out-of-range enum casts are rejected, never
// normalized into unknown text or a different member. Same rules at recorder
// admission, export and load so a successful admission cannot poison export.
[[nodiscard]] core::Result<void> validate_session_event(session::Event event);
[[nodiscard]] core::Result<void> validate_direction(Direction dir);
[[nodiscard]] core::Result<void> validate_audio_role(audio::Role role);
[[nodiscard]] core::Result<void> validate_channel_role(protocol::ChannelRole role);
[[nodiscard]] core::Result<void> validate_wire_id(std::uint64_t id);
[[nodiscard]] core::Result<void> validate_provenance(Provenance provenance);

[[nodiscard]] core::Result<void> validate_audio_format(const audio::AudioFormat& format);
[[nodiscard]] core::Result<void> validate_media_time(core::Nanoseconds timestamp);
[[nodiscard]] core::Result<void> validate_phone_key(std::string_view key);
[[nodiscard]] core::Result<void> validate_states(const Expectation& expect);
[[nodiscard]] core::Result<void> validate_expectation_bounds(const Expectation& expect);
// Full public expectation rule set (states vocabulary/legality, bounds, role
// membership) shared by set_expectation and the fixture loader.
[[nodiscard]] core::Result<void> validate_expectation(
    const Expectation& expect, const std::set<protocol::ChannelRole>& roles);
[[nodiscard]] core::Result<std::set<protocol::ChannelRole>> validate_service_entries(
    const std::vector<ServiceEntry>& services);

// The one canonical body serialization (fixed key order, no whitespace,
// lowercase hex). Export writes it and load requires the file to contain
// exactly its own re-encoding, so a fixture has one byte representation.
[[nodiscard]] std::string encode_canonical_body(const Fixture& fixture);

[[nodiscard]] std::string_view role_name(protocol::ChannelRole role) noexcept;
[[nodiscard]] core::Result<protocol::ChannelRole> parse_role(std::string_view name);
[[nodiscard]] std::string_view audio_role_name(audio::Role role) noexcept;
[[nodiscard]] core::Result<audio::Role> parse_audio_role(std::string_view name);
[[nodiscard]] std::string_view session_op_name(SessionOp op) noexcept;
[[nodiscard]] core::Result<SessionOp> parse_session_op(std::string_view name);

[[nodiscard]] core::Result<void> validate_fixture(const Fixture& fixture);

} // namespace aa::replay::detail
