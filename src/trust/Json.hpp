// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Strict, bounded JSON primitives for the approved-phone store schema
// (task 27). Schema-directed on purpose: only the value shapes the store
// document needs are representable (object/array/string/unsigned integer);
// floats, booleans, null, negative numbers and exotic escapes are rejected at
// parse time, so a tampered store can never smuggle a value past the mapper.
// This is deliberately NOT the replay fixture parser (aa/replay): the trust
// store is foundational and must not depend on the replay/transport stack.

#include <aa/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aa::trust::detail {

struct JsonValue final {
    using Array = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;
    using Storage = std::variant<std::string, std::uint64_t, Array, Object>;

    explicit JsonValue(Storage storage) : storage_(std::move(storage)) {}

    [[nodiscard]] bool is_string() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] bool is_uint() const noexcept { return storage_.index() == 1; }
    [[nodiscard]] bool is_array() const noexcept { return storage_.index() == 2; }
    [[nodiscard]] bool is_object() const noexcept { return storage_.index() == 3; }

    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] std::uint64_t as_uint() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;

private:
    Storage storage_;
};

// Representation budgets: bounded like every other project-owned parser.
struct JsonLimits final {
    std::size_t max_bytes{256 * 1024};
    std::size_t max_depth{8};
    std::size_t max_string{512};
    std::size_t max_array{320};
    std::size_t max_members{16};
};

// Parse one JSON text. Malformed input, limit overruns, duplicate object keys,
// floats, booleans, null, negative numbers and \u escapes are typed
// invalid_argument rejections (fail closed).
[[nodiscard]] core::Result<JsonValue> parse_json(std::string_view text,
                                                 const JsonLimits& limits = {});

// RFC 8259 string escaping (quote, backslash, control bytes).
void append_json_string(std::string& out, std::string_view text);

} // namespace aa::trust::detail
