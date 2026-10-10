// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Versioned on-disk schema for the approved-phone store (task 27), with the
// safe migration path:
//   schema_version 1 (legacy): records carried an opaque matching key only
//                            ({"phone_id": N, "key": "..."}).
//   schema_version 2 (current): records carry the structured strongest
//                            transport identity (wired/wireless/opaque).
// Version 1 loads by migrating each record to an OpaqueIdentity and allocating
// next_phone_id = max(phone_id)+1; the next persist rewrites version 2.
// Any other schema_version, malformed member or unexpected key is a typed
// trust_store_io rejection — a store that cannot be proven safe is never
// silently trusted.
//
// Record rules (BOTH versions, fail closed): exact members only (no extras, no
// missing), record ids in [1, kMaxPhoneId], one phone id per record and ONE
// record per canonical identity key (identity_key()) — a duplicate identity
// could otherwise survive a forget and silently keep reconnect eligibility.
// Version 2 additionally requires next_phone_id strictly greater than every
// record id and at most kMaxPhoneId + 1 (the shared coherent id range; see
// Store.hpp).

#include <aa/core/Result.hpp>
#include <aa/trust/Store.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aa::trust::detail {

inline constexpr std::uint64_t kSchemaVersionLegacy = 1;
inline constexpr std::uint64_t kSchemaVersionCurrent = 2;

struct ParsedStore final {
    std::vector<ApprovedPhone> phones{};
    std::uint64_t next_phone_id{1};
};

// Parse + migrate one store document. Empty text is the empty store. Every
// rejection is trust_store_io (fail closed).
[[nodiscard]] core::Result<ParsedStore> parse_store(std::string_view text);

// Deterministic version-2 serialization (fixed member order).
[[nodiscard]] std::string write_store(const ParsedStore& store);

} // namespace aa::trust::detail
