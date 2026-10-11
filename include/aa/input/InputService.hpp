// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/input/Capability.hpp>
#include <aa/ipc/InputSocket.hpp>

#include <cstdint>
#include <utility>
#include <variant>

namespace aa::input {

// Receiver-side input normalization (todo 26). The display side (task 24)
// already normalizes Qt keyboard/D-pad and mouse input into the ipc::InputEvent
// shape and delivers it over the owner-ruled AF_UNIX input socket; this
// service consumes those events off the InputEndpoint seam and normalizes them
// for the todo-18 channel-services input channel (the AASDK input service
// seam), forwarding each normalized event toward the session/phone fixture.
//
// Receiver-side normalization contract (defined here, consumed here):
//  - keyboard/D-pad: the event source (keyboard_dpad) must be advertised in
//    the capability descriptor's bench sources AND the Android keycode must be
//    advertised in the descriptor's bench keycode set. An advertised key from
//    an advertised source becomes the channel's normalized key event; anything
//    else is refused typed (channel_unsupported_capability), counted, and
//    NEVER forwarded - the descriptor is the single source of truth for what
//    the phone was told.
//  - mouse-to-touch: the event source (mouse_to_touch) must be advertised in
//    the descriptor's bench sources; an unadvertised source is refused typed
//    (channel_unsupported_capability), counted, and NEVER forwarded. For an
//    advertised source the action passes through and the absolute pixel
//    coordinates are normalized into the advertised touch viewport (clamped
//    into [0, width-1] x [0, height-1]). The result is a single-pointer
//    (pointer_id 0) channel touch event - synthesized touch, never a
//    physical-touch claim.
// No vehicle sensor state is ever fabricated here.

// Channel-normalized key event (the AASDK input service seam's key shape).
struct NormalizedKey final {
    std::uint32_t keycode{};  // Android keycode, advertised bench set only
    bool down{};

    [[nodiscard]] friend constexpr bool operator==(const NormalizedKey&,
                                                   const NormalizedKey&) noexcept = default;
};

// Channel-normalized mouse-to-touch event (the AASDK input service seam's
// touch shape): one pointer, viewport-normalized absolute pixels.
struct NormalizedTouch final {
    std::uint32_t x{};           // clamped into [0, viewport.width-1]
    std::uint32_t y{};           // clamped into [0, viewport.height-1]
    std::uint32_t pointer_id{};  // always 0: bench mouse-to-touch is single-pointer
    ipc::TouchAction action{};

    [[nodiscard]] friend constexpr bool operator==(const NormalizedTouch&,
                                                   const NormalizedTouch&) noexcept = default;
};

using NormalizedInput = std::variant<NormalizedKey, NormalizedTouch>;

// The todo-18 channel-services input channel seam (the AASDK input service
// seam): normalized events are handed here for wire encoding toward the
// phone. The session owns the production adapter; the tests' phone fixture
// records what arrives. Non-owning; must outlive the service.
class InputChannelTarget {
public:
    virtual ~InputChannelTarget() = default;
    InputChannelTarget() = default;
    InputChannelTarget(const InputChannelTarget&) = delete;
    InputChannelTarget& operator=(const InputChannelTarget&) = delete;
    InputChannelTarget(InputChannelTarget&&) = delete;
    InputChannelTarget& operator=(InputChannelTarget&&) = delete;

    virtual void on_normalized(const NormalizedInput& event) = 0;
};

// The receiver-side input service. Caller-driven and thread-free: one thread
// at a time submits events. The service OWNS an immutable copy of its
// capability descriptor (gate r2 F1: taken BY VALUE and moved into the owned
// member - no reference ever binds a caller's descriptor, so passing the
// bench_defaults() temporary is safe by construction).
class InputService final {
public:
    InputService(CapabilityDescriptor descriptor, InputChannelTarget& channel)
        : descriptor_(std::move(descriptor)), channel_(channel) {}

    // Consumes one display event (the InputEndpoint::receive seam of the
    // owner-ruled AF_UNIX input socket) and forwards the normalized event to
    // the input channel seam.
    //
    // Error contract: an event whose source is not advertised in the
    // descriptor's bench sources, or a key whose keycode is outside the
    // advertised bench set, is refused channel_unsupported_capability (the
    // capability gate), counted in rejected() and never forwarded. Touch
    // coordinates from an advertised mouse_to_touch source are normalized
    // (clamped), not refused.
    [[nodiscard]] core::Result<void> submit(const ipc::InputEvent& event);

    [[nodiscard]] const CapabilityDescriptor& descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] std::uint64_t forwarded() const noexcept { return forwarded_; }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

private:
    const CapabilityDescriptor descriptor_;
    InputChannelTarget& channel_;
    std::uint64_t forwarded_{0};
    std::uint64_t rejected_{0};
};

} // namespace aa::input
