// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/input/InputService.hpp>

#include <algorithm>

namespace aa::input {
namespace {

// Clamps one absolute-pixel coordinate into [0, extent-1]; a degenerate (zero)
// extent collapses the space to the single coordinate 0.
[[nodiscard]] std::uint32_t clamp_to(std::uint32_t coordinate, std::uint32_t extent) noexcept {
    if (extent == 0) {
        return 0;
    }
    return std::min(coordinate, extent - 1);
}

// True when `source` is advertised in the descriptor's bench input set. The
// descriptor is the single source of truth for event kinds: an event whose
// source is not advertised is refused before any per-kind normalization.
[[nodiscard]] bool supports_source(const CapabilityDescriptor& descriptor, InputSource source) {
    const auto& advertised = descriptor.sources();
    return std::find(advertised.begin(), advertised.end(), source) != advertised.end();
}

// Exhaustive visitor over the InputEvent alternatives: adding a third
// alternative without handling it here is a compile error, never a silent
// fallthrough. Pure normalization - forwarding and counting stay in submit().
// Each overload first requires its event source to be advertised, THEN applies
// the per-kind checks (keycode membership / coordinate normalization).
struct NormalizeVisitor final {
    const CapabilityDescriptor& descriptor;

    [[nodiscard]] core::Result<NormalizedInput> operator()(const ipc::KeyEvent& key) const {
        if (!supports_source(descriptor, InputSource::keyboard_dpad)) {
            return Error{ErrorCode::channel_unsupported_capability};
        }
        const auto& advertised = descriptor.keycodes();
        const bool known =
            std::find(advertised.begin(), advertised.end(), key.keycode) != advertised.end();
        if (!known) {
            return Error{ErrorCode::channel_unsupported_capability};
        }
        return NormalizedInput{NormalizedKey{static_cast<std::uint32_t>(key.keycode),
                                             key.pressed}};
    }

    [[nodiscard]] core::Result<NormalizedInput> operator()(const ipc::TouchEvent& touch) const {
        if (!supports_source(descriptor, InputSource::mouse_to_touch)) {
            return Error{ErrorCode::channel_unsupported_capability};
        }
        const auto& viewport = descriptor.touch_viewport();
        return NormalizedInput{NormalizedTouch{clamp_to(touch.x, viewport.width),
                                               clamp_to(touch.y, viewport.height),
                                               0,
                                               touch.action}};
    }
};

} // namespace

core::Result<void> InputService::submit(const ipc::InputEvent& event) {
    const auto normalized = std::visit(NormalizeVisitor{descriptor_}, event);
    if (!normalized) {
        ++rejected_;
        return normalized.error();
    }
    if (auto sent = channel_.on_normalized(normalized.value()); !sent) {
        ++rejected_;
        return sent.error();
    }
    ++forwarded_;
    return {};
}

} // namespace aa::input
