// SPDX-License-Identifier: GPL-3.0-or-later
// Task 26 receiver-side input normalization: key/mouse events traverse the
// real owner-ruled AF_UNIX input socket and reach the phone fixture
// normalized, through the todo-18 input channel seam. Plus the capability
// gate: unadvertised keycodes are refused typed and never forwarded.
#include "phone_fixture.hpp"

#include "../input_socket/pair.hpp"

#include <aa/input/InputService.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::input;

namespace ipc = aa::ipc;

using inputsensorstest::PhoneFixture;

TEST(InputSensorsInput, KeyAndMouseEventsReachPhoneFixtureNormalizedThroughSocket) {
    // Given: the real task-24 input socket pair and a receiver-side service
    // consuming its endpoint toward the phone fixture.
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    PhoneFixture phone;
    InputService service(CapabilityDescriptor::bench_defaults(), phone);

    // When: a key press/release and a mouse-to-touch sequence cross the socket
    // (the sender connects lazily on its first send).
    ASSERT_TRUE(pair.value().sender.send(ipc::InputEvent{ipc::KeyEvent{19, true}}));
    ASSERT_TRUE(pair.value().sender.send(ipc::InputEvent{ipc::KeyEvent{19, false}}));
    ASSERT_TRUE(pair.value().sender.send(
        ipc::InputEvent{ipc::TouchEvent{10, 20, ipc::TouchAction::down}}));
    ASSERT_TRUE(pair.value().sender.send(
        ipc::InputEvent{ipc::TouchEvent{40, 50, ipc::TouchAction::move}}));
    ASSERT_TRUE(pair.value().sender.send(
        ipc::InputEvent{ipc::TouchEvent{40, 50, ipc::TouchAction::up}}));
    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint);
    for (int index = 0; index < 5; ++index) {
        auto event = endpoint.value().receive(aa::core::Milliseconds{2000});
        ASSERT_TRUE(event);
        ASSERT_TRUE(service.submit(event.value()));
    }

    // Then: the phone fixture received the channel-normalized events through
    // the input channel seam.
    const std::vector<NormalizedInput> expected{
        NormalizedKey{19, true},
        NormalizedKey{19, false},
        NormalizedTouch{10, 20, 0, ipc::TouchAction::down},
        NormalizedTouch{40, 50, 0, ipc::TouchAction::move},
        NormalizedTouch{40, 50, 0, ipc::TouchAction::up},
    };
    EXPECT_EQ(phone.input, expected);
    EXPECT_EQ(phone.sensors.size(), 0U);
    EXPECT_EQ(service.forwarded(), 5U);
    EXPECT_EQ(service.rejected(), 0U);
    EXPECT_EQ(endpoint.value().rejected(), 0U);
}

TEST(InputSensorsInput, MouseCoordinatesNormalizeIntoAdvertisedViewport) {
    // Given: the bench service with its advertised 1280x720 touch viewport.
    PhoneFixture phone;
    InputService service(CapabilityDescriptor::bench_defaults(), phone);

    // When: a mouse-to-touch event carries pixels outside the viewport.
    ASSERT_TRUE(service.submit(
        ipc::InputEvent{ipc::TouchEvent{2000, 900, ipc::TouchAction::move}}));

    // Then: the coordinates are normalized (clamped) into the advertised
    // viewport - synthesized single-pointer touch, never a physical claim.
    ASSERT_EQ(phone.input.size(), 1U);
    EXPECT_EQ(phone.input.front(),
              (NormalizedInput{NormalizedTouch{1279, 719, 0, ipc::TouchAction::move}}));
}

TEST(InputSensorsInput, EveryAdvertisedKeycodePassesTheCapabilityGate) {
    // Given: the bench service and the descriptor's advertised keycode set.
    PhoneFixture phone;
    const auto descriptor = CapabilityDescriptor::bench_defaults();
    InputService service(descriptor, phone);

    // When: one press event is submitted per advertised keycode.
    for (const std::int32_t keycode : descriptor.keycodes()) {
        ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{keycode, true}}));
    }

    // Then: every advertised key reached the phone fixture normalized.
    EXPECT_EQ(phone.input.size(), descriptor.keycodes().size());
    EXPECT_EQ(service.forwarded(), descriptor.keycodes().size());
    EXPECT_EQ(service.rejected(), 0U);
    for (std::size_t index = 0; index < descriptor.keycodes().size(); ++index) {
        EXPECT_EQ(phone.input[index],
                  (NormalizedInput{NormalizedKey{
                      static_cast<std::uint32_t>(descriptor.keycodes()[index]), true}}));
    }
}

TEST(InputSensorsInput, UnadvertisedKeycodeIsRefusedTypedAndNeverForwarded) {
    // Given: the bench service (the advertised bench set has no keycode 999).
    PhoneFixture phone;
    InputService service(CapabilityDescriptor::bench_defaults(), phone);

    // When: an unadvertised keycode is submitted.
    const auto refused = service.submit(ipc::InputEvent{ipc::KeyEvent{999, true}});

    // Then: the capability gate refuses it typed and the phone fixture sees
    // nothing.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::channel_unsupported_capability);
    EXPECT_TRUE(phone.input.empty());
    EXPECT_EQ(service.forwarded(), 0U);
    EXPECT_EQ(service.rejected(), 1U);

    // And: the service stays usable for the advertised set afterwards.
    ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{19, true}}));
    ASSERT_EQ(phone.input.size(), 1U);
    EXPECT_EQ(phone.input.front(), (NormalizedInput{NormalizedKey{19, true}}));
    EXPECT_EQ(service.forwarded(), 1U);
    EXPECT_EQ(service.rejected(), 1U);
}

TEST(InputSensorsInput, TouchWithoutMouseSourceIsRefusedTypedAndNeverForwarded) {
    // Given: a service whose descriptor advertises keyboard_dpad ONLY (no
    // mouse_to_touch source), even though its keycodes and viewport are set.
    PhoneFixture phone;
    const CapabilityDescriptor keyboard_only{{InputSource::keyboard_dpad},
                                            {19},
                                            TouchViewport{1280, 720},
                                            {},
                                            NightModePolicy::manual};
    InputService service(keyboard_only, phone);

    // When: a valid touch event is submitted from the unadvertised source.
    const auto refused =
        service.submit(ipc::InputEvent{ipc::TouchEvent{10, 20, ipc::TouchAction::down}});

    // Then: the capability gate refuses it typed and the phone fixture sees
    // nothing.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::channel_unsupported_capability);
    EXPECT_TRUE(phone.input.empty());
    EXPECT_EQ(service.forwarded(), 0U);
    EXPECT_EQ(service.rejected(), 1U);

    // And: an event from the advertised source forwards WITHOUT resetting the
    // rejection count.
    ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{19, true}}));
    ASSERT_EQ(phone.input.size(), 1U);
    EXPECT_EQ(phone.input.front(), (NormalizedInput{NormalizedKey{19, true}}));
    EXPECT_EQ(service.forwarded(), 1U);
    EXPECT_EQ(service.rejected(), 1U);
}

TEST(InputSensorsInput, AdvertisedKeyWithoutKeyboardSourceIsRefusedTypedAndNeverForwarded) {
    // Given: a service whose descriptor advertises mouse_to_touch ONLY (no
    // keyboard_dpad source), even though keycode 19 IS in its keycode set.
    PhoneFixture phone;
    const CapabilityDescriptor touch_only{{InputSource::mouse_to_touch},
                                         {19},
                                         TouchViewport{1280, 720},
                                         {},
                                         NightModePolicy::manual};
    InputService service(touch_only, phone);

    // When: an advertised keycode is submitted from the unadvertised source.
    const auto refused = service.submit(ipc::InputEvent{ipc::KeyEvent{19, true}});

    // Then: the capability gate refuses it typed and the phone fixture sees
    // nothing.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::channel_unsupported_capability);
    EXPECT_TRUE(phone.input.empty());
    EXPECT_EQ(service.forwarded(), 0U);
    EXPECT_EQ(service.rejected(), 1U);

    // And: an event from the advertised source forwards WITHOUT resetting the
    // rejection count.
    ASSERT_TRUE(service.submit(
        ipc::InputEvent{ipc::TouchEvent{10, 20, ipc::TouchAction::down}}));
    ASSERT_EQ(phone.input.size(), 1U);
    EXPECT_EQ(phone.input.front(),
              (NormalizedInput{NormalizedTouch{10, 20, 0, ipc::TouchAction::down}}));
    EXPECT_EQ(service.forwarded(), 1U);
    EXPECT_EQ(service.rejected(), 1U);
}

TEST(InputSensorsInput, BenchDefaultsAcceptDelAndEnterKeycodes) {
    // Given: the bench service (the advertised set now carries 66 ENTER and
    // 67 DEL, the corrected Backspace/Return semantics).
    PhoneFixture phone;
    InputService service(CapabilityDescriptor::bench_defaults(), phone);

    // When: DEL (67) and ENTER (66) press/release pairs are submitted.
    ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{67, true}}));
    ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{67, false}}));
    ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{66, true}}));
    ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{66, false}}));

    // Then: all four reach the phone fixture normalized.
    const std::vector<NormalizedInput> expected{
        NormalizedKey{67, true},
        NormalizedKey{67, false},
        NormalizedKey{66, true},
        NormalizedKey{66, false},
    };
    EXPECT_EQ(phone.input, expected);
    EXPECT_EQ(service.forwarded(), 4U);
    EXPECT_EQ(service.rejected(), 0U);
}

TEST(InputSensorsInput, BenchDefaultsRejectBackAndDpadCenterKeycodes) {
    // Given: the bench service (no remaining widget mapping produces BACK 4
    // or DPAD_CENTER 23, so neither is advertised).
    PhoneFixture phone;
    InputService service(CapabilityDescriptor::bench_defaults(), phone);

    // When: keycode 4 and keycode 23 are submitted.
    const auto refused_back = service.submit(ipc::InputEvent{ipc::KeyEvent{4, true}});
    const auto refused_center = service.submit(ipc::InputEvent{ipc::KeyEvent{23, true}});

    // Then: the capability gate refuses both typed and the phone fixture sees
    // nothing.
    ASSERT_FALSE(refused_back);
    EXPECT_EQ(refused_back.error().code(), aa::ErrorCode::channel_unsupported_capability);
    ASSERT_FALSE(refused_center);
    EXPECT_EQ(refused_center.error().code(), aa::ErrorCode::channel_unsupported_capability);
    EXPECT_TRUE(phone.input.empty());
    EXPECT_EQ(service.forwarded(), 0U);
    EXPECT_EQ(service.rejected(), 2U);
}

TEST(InputSensorsInput, OwnedDescriptorOutlivesCallerHeapDestruction) {
    // Given: a heap-allocated descriptor handed to the service and FREED
    // before the service is used again (the phone target stays alive).
    PhoneFixture phone;
    std::unique_ptr<CapabilityDescriptor> heap_descriptor = std::make_unique<CapabilityDescriptor>(
        CapabilityDescriptor{
            {InputSource::keyboard_dpad, InputSource::mouse_to_touch},
            {19, 67},
            TouchViewport{640, 480},
            {},
            NightModePolicy::manual,
        });
    InputService service(*heap_descriptor, phone);
    heap_descriptor.reset();

    // When: the service's descriptor and API are exercised after the caller's
    // heap block is gone.
    const CapabilityDescriptor expected{
        {InputSource::keyboard_dpad, InputSource::mouse_to_touch},
        {19, 67},
        TouchViewport{640, 480},
        {},
        NightModePolicy::manual,
    };
    EXPECT_EQ(service.descriptor(), expected);
    ASSERT_TRUE(service.submit(ipc::InputEvent{ipc::KeyEvent{67, true}}));
    ASSERT_TRUE(service.submit(
        ipc::InputEvent{ipc::TouchEvent{900, 900, ipc::TouchAction::down}}));

    // Then: the service read its OWN descriptor copy - sources, keycodes and
    // viewport all resolve after the caller's destruction. A borrowed
    // descriptor would be a definite freed-heap use-after-free under
    // memcheck here.
    ASSERT_EQ(phone.input.size(), 2U);
    EXPECT_EQ(phone.input[0], (NormalizedInput{NormalizedKey{67, true}}));
    EXPECT_EQ(phone.input[1],
              (NormalizedInput{NormalizedTouch{639, 479, 0, ipc::TouchAction::down}}));
    EXPECT_EQ(service.forwarded(), 2U);
    EXPECT_EQ(service.rejected(), 0U);
}

} // namespace
