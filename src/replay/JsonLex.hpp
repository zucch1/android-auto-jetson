// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// JSON scalar lexing (string literals with escapes, numbers) split from the
// structural parser. Bounded: string lengths are capped and malformed escapes
// or numbers are typed invalid_argument rejections.

#include <aa/core/Result.hpp>

#include "Json.hpp"

#include <cstddef>
#include <string>

namespace aa::replay::detail {

// Consumes one string literal at text[pos] (pos points at the opening quote).
[[nodiscard]] core::Result<std::string> lex_string(std::string_view text, std::size_t& pos,
                                                   const JsonLimits& limits);

// Consumes one JSON number at text[pos].
[[nodiscard]] core::Result<JsonValue> lex_number(std::string_view text, std::size_t& pos);

// RFC 3629 well-formedness for raw text (no overlong forms, surrogates or
// out-of-range scalars). Same rule the lexer enforces inside JSON strings, so
// recorder-side text checks and the parser boundary cannot diverge.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

} // namespace aa::replay::detail
