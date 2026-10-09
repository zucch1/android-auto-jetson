// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/replay/Fixture.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace aa::replay {

// Fixture schema boundary: one export writer and one loader, both bounded and
// deterministic. The export gate routes recorder metadata through the task-19
// diagnostics serializer (to_json) and then fixed-redacts the salted
// pseudonyms ("id-<16 hex>") and identifier shapes so fixture hashes never
// depend on process-salted FNV output. The loader re-proves the metadata
// survived that boundary (unknown keys and invalid values cannot round-trip)
// and rejects raw identifier shapes, oversized counts/payload/time,
// nonmonotonic timestamps, wrong schema, inconsistent IDs/metadata and
// illegal state traces (checked against the real session transition table).
//
// LIMITATION: the gate covers metadata text only. Opaque record payloads are
// not and cannot be privacy-scrubbed; they are admitted solely under the
// explicit synthetic opaque policy.

// Serialize one fixture to the canonical file form and compute its SHA-256
// over the fixed canonical body bytes. Enforces every recorder bound.
[[nodiscard]] core::Result<ExportedFixture> export_fixture(const Fixture& fixture);

// Parse and validate one fixture file (exact canonical envelope required).
// Typed rejections: wrong schema / malformed shape / inconsistent IDs or
// metadata / nonmonotonic timestamps -> invalid_argument; illegal state trace
// -> session_illegal_transition; oversized payload bytes ->
// transport_oversize_frame; oversized counts/time -> invalid_argument; hash
// mismatch (corrupted fixture) -> invalid_argument.
[[nodiscard]] core::Result<Fixture> load_fixture(std::span<const std::byte> file_bytes);

// Full QA gate: load plus the metadata privacy re-checks. Used by the manual
// driver and the Python QA as the authoritative accept/reject.
[[nodiscard]] core::Result<void> check_fixture(std::span<const std::byte> file_bytes);

// Fixed-redaction pass over one to_json output line: replaces the salted
// pseudonyms to_json may emit ("id-<16 hex>") with the fixed marker
// "[redacted]" so exported metadata is process-independent. Identifier shapes
// in field values are rejected separately (they must never reach export).
[[nodiscard]] std::string fixedredact_metadata(std::string_view to_json_line);

// True when one piece of text carries a raw identifier shape (MAC or a 15+
// digit run as IMEI/IMSI/MEID serials do). Used by the recorder to
// fixed-redact before serialization and by the gates to reject.
[[nodiscard]] bool has_identifier_shape(std::string_view text) noexcept;

// Rejects metadata text that still carries a raw identifier shape anywhere in
// a field value (envelope keys session/mono_ns excepted: they are recorder
// counters, not identifiers).
[[nodiscard]] core::Result<void> reject_raw_identifiers(std::string_view to_json_line);

} // namespace aa::replay
