// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>

#include <limits>

namespace aa::ipc {
namespace {

void put(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value,
         unsigned size) noexcept {
    for (unsigned i = 0; i < size; ++i) {
        out[offset + i] = static_cast<std::uint8_t>(value >> (8 * (size - i - 1)));
    }
}

template <unsigned size>
std::uint64_t get(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        value = (value << 8) | in[offset + i];
    }
    return value;
}

Error malformed() noexcept { return Error{ErrorCode::transport_malformed_frame}; }
Error oversize() noexcept { return Error{ErrorCode::transport_oversize_frame}; }

} // namespace

void encode_input_record(std::array<std::uint8_t, kInputRecordBytes>& record,
                         const InputEvent& event) noexcept {
    std::span<std::uint8_t> out{record.data(), record.size()};
    InputRecordHeader header{};
    if (const auto* key = std::get_if<KeyEvent>(&event)) {
        header.kind = kInputKindKey;
        header.a = static_cast<std::uint32_t>(key->keycode);
        header.b = key->pressed ? 1U : 0U;
        header.c = 0;
    } else {
        const auto& touch = std::get<TouchEvent>(event);
        header.kind = kInputKindTouch;
        header.a = touch.x;
        header.b = touch.y;
        header.c = static_cast<std::uint32_t>(touch.action);
    }
    put(out, 0, kInputRecordMagic, 4);
    put(out, 4, kInputProtocolVersion, 2);
    put(out, 6, kInputRecordBytes, 2);
    put(out, 8, header.kind, 4);
    put(out, 12, header.a, 4);
    put(out, 16, header.b, 4);
    put(out, 20, header.c, 4);
}

core::Result<InputEvent> decode_input_record(std::span<const std::uint8_t> record) noexcept {
    if (record.size() < kInputRecordBytes) {
        return malformed();
    }
    if (record.size() > kInputRecordBytes) {
        return oversize();
    }
    if (get<4>(record, 0) != kInputRecordMagic || get<2>(record, 6) != kInputRecordBytes) {
        return malformed();
    }
    if (get<2>(record, 4) != kInputProtocolVersion) {
        return Error{ErrorCode::protocol_version_mismatch};
    }
    const auto kind = get<4>(record, 8);
    const auto a = get<4>(record, 12);
    const auto b = get<4>(record, 16);
    const auto c = get<4>(record, 20);
    if (kind == kInputKindKey) {
        if (b > 1 || c != 0 || a > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            return malformed();
        }
        return InputEvent{KeyEvent{static_cast<std::int32_t>(a), b == 1}};
    }
    if (kind == kInputKindTouch) {
        if (c > static_cast<std::uint32_t>(TouchAction::up)) {
            return malformed();
        }
        return InputEvent{TouchEvent{static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b),
                                     static_cast<TouchAction>(c)}};
    }
    return malformed();
}

} // namespace aa::ipc
