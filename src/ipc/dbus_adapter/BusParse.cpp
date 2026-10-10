// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusWire.hpp"

namespace aa::ipc::dbus_adapter {

namespace {

constexpr int kMaxTypeDepth = 16;

// End index of one complete type in `sig` starting at `pos`, or 0 on error.
std::size_t complete_type_end(std::string_view sig, std::size_t pos, int depth) {
    if (depth > kMaxTypeDepth || pos >= sig.size()) {
        return 0;
    }
    switch (sig[pos]) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u':
    case 'x': case 't': case 'd': case 's': case 'o': case 'g': case 'v': case 'h':
        return pos + 1;
    case 'a':
        return complete_type_end(sig, pos + 1, depth + 1);
    case '(': {
        std::size_t cursor = pos + 1;
        while (cursor < sig.size() && sig[cursor] != ')') {
            cursor = complete_type_end(sig, cursor, depth + 1);
            if (cursor == 0) {
                return 0;
            }
        }
        return cursor < sig.size() ? cursor + 1 : 0;
    }
    case '{': {
        if (pos + 1 >= sig.size() || std::string_view{"ybnqiuxtdsog"}
                                        .find(sig[pos + 1]) == std::string_view::npos) {
            return 0;
        }
        const std::size_t value_end = complete_type_end(sig, pos + 2, depth + 1);
        if (value_end == 0 || value_end >= sig.size() || sig[value_end] != '}') {
            return 0;
        }
        return value_end + 1;
    }
    default:
        return 0;
    }
}

} // namespace

core::Result<void> Cursor::align(std::size_t boundary) {
    if (boundary == 0 || (boundary & (boundary - 1)) != 0) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    while (pos_ % boundary != 0) {
        if (pos_ >= data_.size()) {
            return Error{ErrorCode::transport_malformed_frame};
        }
        ++pos_;
    }
    return {};
}

core::Result<std::uint8_t> Cursor::read_u8() {
    if (pos_ >= data_.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    return data_[pos_++];
}

core::Result<std::uint32_t> Cursor::read_u32() {
    const auto aligned = align(4);
    if (!aligned.has_value()) {
        return aligned.error();
    }
    if (pos_ + 4 > data_.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(data_[pos_++]) << shift;
    }
    return value;
}

core::Result<std::uint64_t> Cursor::read_u64() {
    const auto aligned = align(8);
    if (!aligned.has_value()) {
        return aligned.error();
    }
    if (pos_ + 8 > data_.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(data_[pos_++]) << shift;
    }
    return value;
}

core::Result<std::string> Cursor::read_string() {
    const auto length = read_u32();
    if (!length.has_value()) {
        return length.error();
    }
    if (pos_ + length.value() + 1 > data_.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    std::string value(reinterpret_cast<const char*>(data_.data() + pos_), length.value());
    pos_ += length.value();
    if (data_[pos_] != 0) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    ++pos_;
    return value;
}

core::Result<std::string> Cursor::read_signature() {
    const auto length = read_u8();
    if (!length.has_value()) {
        return length.error();
    }
    if (pos_ + static_cast<std::size_t>(length.value()) + 1 > data_.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    std::string value(reinterpret_cast<const char*>(data_.data() + pos_), length.value());
    pos_ += length.value();
    if (data_[pos_] != 0) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    ++pos_;
    return value;
}

core::Result<void> Cursor::skip_value(std::string_view type) {
    const std::size_t end = complete_type_end(type, 0, 0);
    if (end == 0 || end != type.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    switch (type[0]) {
    case 'y': {
        const auto value = read_u8();
        return value.has_value() ? core::Result<void>{} : core::Result<void>{value.error()};
    }
    case 'g': {
        const auto value = read_signature();
        return value.has_value() ? core::Result<void>{} : core::Result<void>{value.error()};
    }
    case 'n':
    case 'q': {
        const auto aligned = align(2);
        if (!aligned.has_value()) {
            return aligned.error();
        }
        if (pos_ + 2 > data_.size()) {
            return Error{ErrorCode::transport_malformed_frame};
        }
        pos_ += 2;
        return {};
    }
    case 'b':
    case 'i':
    case 'u': {
        const auto value = read_u32();
        return value.has_value() ? core::Result<void>{} : core::Result<void>{value.error()};
    }
    case 'x':
    case 't':
    case 'd': {
        const auto value = read_u64();
        return value.has_value() ? core::Result<void>{} : core::Result<void>{value.error()};
    }
    case 's':
    case 'o': {
        const auto value = read_string();
        return value.has_value() ? core::Result<void>{} : core::Result<void>{value.error()};
    }
    case 'v': {
        const auto sig = read_signature();
        if (!sig.has_value()) {
            return sig.error();
        }
        return skip_value(sig.value());
    }
    case 'a': {
        const auto length = read_u32();
        if (!length.has_value()) {
            return length.error();
        }
        const std::string_view element = type.substr(1);
        std::size_t first = 0;
        while (first < element.size() && element[first] == 'a') {
            ++first;
        }
        std::size_t boundary = 1;
        if (first < element.size()) {
            switch (element[first]) {
            case '(': case '{': case 'x': case 't': case 'd': boundary = 8; break;
            case 'b': case 'i': case 'u': case 's': case 'o': boundary = 4; break;
            case 'n': case 'q': boundary = 2; break;
            default: boundary = 1; break;
            }
        }
        const auto aligned = align(boundary);
        if (!aligned.has_value()) {
            return aligned.error();
        }
        const std::size_t stop = pos_ + length.value();
        if (stop > data_.size()) {
            return Error{ErrorCode::transport_malformed_frame};
        }
        while (pos_ < stop) {
            const auto skipped = skip_value(element);
            if (!skipped.has_value()) {
                return skipped.error();
            }
        }
        if (pos_ != stop) {
            return Error{ErrorCode::transport_malformed_frame};
        }
        return {};
    }
    case '(': {
        const auto aligned = align(8);
        if (!aligned.has_value()) {
            return aligned.error();
        }
        std::size_t cursor = 1;
        while (cursor < type.size() && type[cursor] != ')') {
            const std::size_t member_end = complete_type_end(type, cursor, 1);
            if (member_end == 0 || member_end > type.size()) {
                return Error{ErrorCode::transport_malformed_frame};
            }
            const auto skipped = skip_value(type.substr(cursor, member_end - cursor));
            if (!skipped.has_value()) {
                return skipped.error();
            }
            cursor = member_end;
        }
        return {};
    }
    case '{': {
        const auto aligned = align(8);
        if (!aligned.has_value()) {
            return aligned.error();
        }
        const std::size_t key_end = complete_type_end(type, 1, 1);
        if (key_end == 0 || key_end + 1 >= type.size() || type[type.size() - 1] != '}') {
            return Error{ErrorCode::transport_malformed_frame};
        }
        const auto skipped_key = skip_value(type.substr(1, key_end - 1));
        if (!skipped_key.has_value()) {
            return skipped_key.error();
        }
        return skip_value(type.substr(key_end, type.size() - key_end - 1));
    }
    default:
        return Error{ErrorCode::transport_malformed_frame};
    }
}

} // namespace aa::ipc::dbus_adapter
