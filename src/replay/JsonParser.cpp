// SPDX-License-Identifier: GPL-3.0-or-later
#include "Json.hpp"

#include "JsonLex.hpp"

#include <string_view>

namespace aa::replay::detail {
namespace {

constexpr Error bad_json() { return Error{ErrorCode::invalid_argument}; }

class Parser final {
public:
    Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

    core::Result<JsonValue> parse_document() {
        auto value = parse_value(0);
        if (!value) {
            return value.error();
        }
        if (pos_ != text_.size()) {
            return bad_json();
        }
        return std::move(value).value();
    }

private:
    core::Result<JsonValue> parse_value(std::size_t depth) {
        if (depth > limits_.max_depth || nodes_ >= limits_.max_nodes) {
            return bad_json();
        }
        ++nodes_;
        skip_space();
        if (pos_ >= text_.size()) {
            return bad_json();
        }
        switch (text_[pos_]) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': {
            auto text = lex_string(text_, pos_, limits_);
            if (!text) {
                return text.error();
            }
            return JsonValue{JsonValue::Storage{std::move(text).value()}};
        }
        case 't': return parse_literal("true", JsonValue::Storage{true});
        case 'f': return parse_literal("false", JsonValue::Storage{false});
        case 'n': return parse_literal("null", JsonValue::Storage{});
        default: return lex_number(text_, pos_);
        }
    }

    core::Result<JsonValue> parse_literal(std::string_view literal, JsonValue::Storage storage) {
        if (text_.substr(pos_, literal.size()) != literal) {
            return bad_json();
        }
        pos_ += literal.size();
        return JsonValue{std::move(storage)};
    }

    core::Result<JsonValue> parse_array(std::size_t depth) {
        ++pos_; // '['
        JsonValue::Array items;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return JsonValue{JsonValue::Storage{std::move(items)}};
        }
        while (true) {
            if (items.size() >= limits_.max_elements) {
                return bad_json();
            }
            auto item = parse_value(depth + 1);
            if (!item) {
                return item.error();
            }
            items.push_back(std::move(item).value());
            skip_space();
            if (pos_ >= text_.size()) {
                return bad_json();
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return JsonValue{JsonValue::Storage{std::move(items)}};
            }
            if (text_[pos_] != ',') {
                return bad_json();
            }
            ++pos_;
        }
    }

    core::Result<JsonValue> parse_object(std::size_t depth) {
        ++pos_; // '{'
        JsonValue::Object members;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return JsonValue{JsonValue::Storage{std::move(members)}};
        }
        while (true) {
            if (members.size() >= limits_.max_elements) {
                return bad_json();
            }
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return bad_json();
            }
            auto key = lex_string(text_, pos_, limits_);
            if (!key) {
                return key.error();
            }
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return bad_json();
            }
            ++pos_;
            auto value = parse_value(depth + 1);
            if (!value) {
                return value.error();
            }
            members.emplace_back(std::move(key).value(), std::move(value).value());
            skip_space();
            if (pos_ >= text_.size()) {
                return bad_json();
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return JsonValue{JsonValue::Storage{std::move(members)}};
            }
            if (text_[pos_] != ',') {
                return bad_json();
            }
            ++pos_;
        }
    }

    void skip_space() {
        while (pos_ < text_.size()
               && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n'
                   || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    std::string_view text_;
    const JsonLimits& limits_;
    std::size_t pos_{};
    std::size_t nodes_{};
};

} // namespace

core::Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits) {
    if (text.size() > limits.max_document) {
        return bad_json();
    }
    Parser parser(text, limits);
    return parser.parse_document();
}

} // namespace aa::replay::detail
