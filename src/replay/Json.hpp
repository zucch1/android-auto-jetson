// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounded JSON value parser/writer for the fixture schema and the metadata
// privacy gate. Strict RFC 8259 subset: no unbounded allocation (node, depth,
// string and container limits), ordered object members, int64/double/bool/
// string/array/object/null values.

#include <aa/core/Result.hpp>
#include <aa/replay/Fixture.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aa::replay::detail {

// Representation budgets derived from the accepted fixture bounds (Fixture.hpp)
// so anything exportable stays parseable: a single string and the whole
// document cannot exceed the file cap, and the node/container budgets exceed
// the widest valid fixture by a fixed margin.
struct JsonLimits final {
    std::size_t max_depth{8};
    std::size_t max_nodes{kMaxRecords * 64};
    std::size_t max_string{kMaxFixtureBytes};
    std::size_t max_elements{kMaxRecords * 2};
    std::size_t max_document{kMaxFixtureBytes};
};

class JsonValue final {
public:
    using Array = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, Array,
                                 Object>;

    JsonValue() = default;
    explicit JsonValue(Storage storage) : storage_(std::move(storage)) {}

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_int() const noexcept;
    [[nodiscard]] bool is_real() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::int64_t as_int() const;
    [[nodiscard]] double as_real() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;

    // Numeric value as int64 when the JSON number is integral.
    [[nodiscard]] bool int_value(std::int64_t& out) const;

private:
    Storage storage_{};
};

// Parse one JSON text. Malformed input, limit overruns and trailing garbage
// are typed invalid_argument rejections.
[[nodiscard]] core::Result<JsonValue> parse_json(std::string_view text,
                                                 const JsonLimits& limits = {});

// RFC 8259 string escaping (quote, backslash, control bytes).
void write_json_string(std::string& out, std::string_view text);

} // namespace aa::replay::detail
