// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Error.hpp>

#include <utility>
#include <variant>

namespace aa::core {

// C++20 result type: exactly one of a value or a typed Error, stored in a
// std::variant. There is no C++23 std::expected in this codebase. Callers must
// inspect has_value() before value()/error(); accessing the wrong alternative
// throws std::bad_variant_access and is a contract violation, not a recovery
// path. Construction is implicit from either alternative so call sites read
// `return value;` / `return Error{...};`.
template <class T>
class [[nodiscard]] Result final {
public:
    using value_type = T;

    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, error) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

private:
    std::variant<T, Error> storage_;
};

// Void specialization keeps the same variant discipline: the success
// alternative is std::monostate.
template <>
class [[nodiscard]] Result<void> final {
public:
    using value_type = void;

    Result() : storage_(std::in_place_index<0>, std::monostate{}) {}
    Result(Error error) : storage_(std::in_place_index<1>, error) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

private:
    std::variant<std::monostate, Error> storage_;
};

} // namespace aa::core
