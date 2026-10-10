// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <string_view>

namespace aa::replay::detail {

// The metadata privacy gate: the stored to_json line must round-trip exactly
// through the task-19 serializer (so unknown keys, invalid values and raw
// secrets cannot survive) and must carry no raw identifier shape in any field
// value. Used by both export and load; it is the schema authority for the
// metadata section.
[[nodiscard]] core::Result<void> metadata_gate(std::string_view to_json_line);

} // namespace aa::replay::detail
