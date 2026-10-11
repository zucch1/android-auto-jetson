// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/input/Capability.hpp>

namespace aa::input {
namespace {

// The keyboard/D-pad keycode space the task-24 dummy display actually
// normalizes (src/display/qt_adapter/ProjectionWidget.cpp android_keycode):
// backspace, digits 0-9, D-pad + enter, letters A-Z, space, escape. The
// descriptor advertises exactly this set ("actual non-touch buttons, no
// defaults"): anything else is refused at the capability gate.
constexpr std::int32_t kBackspace = 4;
constexpr std::int32_t kDigitZero = 7;
constexpr std::int32_t kDigitNine = 16;
constexpr std::int32_t kDpadUp = 19;
constexpr std::int32_t kEnter = 23;
constexpr std::int32_t kLetterA = 29;
constexpr std::int32_t kLetterZ = 54;
constexpr std::int32_t kSpace = 62;
constexpr std::int32_t kEscape = 111;

} // namespace

CapabilityDescriptor CapabilityDescriptor::bench_defaults() {
    std::vector<std::int32_t> keycodes{};
    keycodes.push_back(kBackspace);
    for (std::int32_t digit = kDigitZero; digit <= kDigitNine; ++digit) {
        keycodes.push_back(digit);
    }
    for (std::int32_t key = kDpadUp; key <= kEnter; ++key) {
        keycodes.push_back(key);
    }
    for (std::int32_t letter = kLetterA; letter <= kLetterZ; ++letter) {
        keycodes.push_back(letter);
    }
    keycodes.push_back(kSpace);
    keycodes.push_back(kEscape);
    return CapabilityDescriptor{
        {InputSource::keyboard_dpad, InputSource::mouse_to_touch},
        std::move(keycodes),
        TouchViewport{1280, 720},
        {},
        NightModePolicy::manual,
    };
}

} // namespace aa::input
