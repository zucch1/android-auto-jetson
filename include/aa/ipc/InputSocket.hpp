// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

namespace aa::ipc {

// Normalized display input (the todo-26 input shape, coordinated here so the
// receiver-side endpoint todo 26 consumes shares one definition with the
// display side): keyboard/D-pad key events with Android keycodes and
// mouse-to-touch events in absolute viewport pixels. No vehicle sensor state
// is ever fabricated here. The display's Qt surface normalizes its input into
// this shape and delivers it to an InputSink; the input socket (InputChannel)
// carries it to the receiver, where todo 26's input service will consume the
// endpoint.
enum class TouchAction : std::uint8_t { down, move, up };

struct KeyEvent final {
    std::int32_t keycode{};
    bool pressed{};

    [[nodiscard]] friend constexpr bool operator==(const KeyEvent&, const KeyEvent&) noexcept =
        default;
};

struct TouchEvent final {
    std::uint32_t x{};
    std::uint32_t y{};
    TouchAction action{};

    [[nodiscard]] friend constexpr bool operator==(const TouchEvent&, const TouchEvent&) noexcept =
        default;
};

using InputEvent = std::variant<KeyEvent, TouchEvent>;

// Delivery seam for normalized input events. In-memory sinks are test
// artifacts only; the shipped display wires the socket sender as its sink.
class InputSink {
public:
    virtual ~InputSink() = default;
    InputSink() = default;
    InputSink(const InputSink&) = delete;
    InputSink& operator=(const InputSink&) = delete;
    InputSink(InputSink&&) = delete;
    InputSink& operator=(InputSink&&) = delete;

    virtual void on_input(const InputEvent& event) = 0;
};

// AF_UNIX SOCK_SEQPACKET display-input channel (owner ruling 2026-10-10,
// .omo/evidence/jetson-android-auto-receiver/execution-input-transport-decision.json):
// a NEW bounded socket in the task-22 surface family, served from
// /run/user/$UID/android-auto-receiver/input.sock. Input is low-rate and every
// event is ONE fixed-size datagram: the framing is bounded by construction,
// malformed and oversize records are rejected with typed errors
// (transport_malformed_frame / transport_oversize_frame, version drift with
// protocol_version_mismatch) and no receive path allocates beyond a fixed
// stack record. The task-21 D-Bus contract stays control-only and its XML is
// NOT extended with input methods - this socket is the input transport.
//
// Ownership/threading: the receiver binds and accepts (InputServer ->
// InputEndpoint, the typed seam todo 26 consumes); the display connects and
// sends (InputSender). One endpoint per accepted sender. Caller-driven and
// thread-free: send on one thread, receive on another, no internal threads.
//
// Peer policy mirrors the video socket exactly (SO_PEERCRED, same-UID only,
// fail closed): unauthorized peers are rejected typed
// (transport_unauthorized_peer) and counted; a sender also verifies the
// server's UID before use.
inline constexpr std::size_t kInputRecordBytes = 24;
inline constexpr std::uint16_t kInputProtocolVersion = 1;
inline constexpr std::uint32_t kInputRecordMagic = 0x41414931U; // "AAI1"

// Record kinds (offset 8).
inline constexpr std::uint32_t kInputKindKey = 1;
inline constexpr std::uint32_t kInputKindTouch = 2;

// Fixed 24-byte wire record (big-endian, field order is the layout):
//   offset  0: magic (kInputRecordMagic)
//   offset  4: protocol version (u16), record size (u16 = 24)
//   offset  8: kind (u32: 1 key, 2 touch)
//   offset 12: a (u32: key -> keycode, touch -> x)
//   offset 16: b (u32: key -> pressed 0|1, touch -> y)
//   offset 20: c (u32: key -> 0, touch -> action 0|1|2)
struct InputRecordHeader final {
    std::uint32_t kind{};
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
};

// Pure wire codec (no sockets). Every decode failure is a typed error; no path
// allocates beyond the caller-supplied span and the returned event.
void encode_input_record(std::array<std::uint8_t, kInputRecordBytes>& record,
                         const InputEvent& event) noexcept;
[[nodiscard]] core::Result<InputEvent>
decode_input_record(std::span<const std::uint8_t> record) noexcept;

struct InputSocketPath final {
    std::string value{};

    [[nodiscard]] friend bool operator==(const InputSocketPath&, const InputSocketPath&) noexcept =
        default;
};

[[nodiscard]] InputSocketPath production_input_socket_path();
[[nodiscard]] InputSocketPath input_socket_path_in(const std::string& runtime_dir);

// Same path policy as the video socket (task-22 family): create the directory
// (real directory, owned by the caller, 0700) and clear the endpoint path only
// when an existing socket node is verified stale (non-blocking datagram probe:
// connect refused). A live endpoint or a non-socket node at the path is
// refused `transport_unavailable` and left untouched. Malformed path ->
// invalid_argument; directory failures -> transport_unavailable.
[[nodiscard]] core::Result<void> prepare_input_socket_path(const InputSocketPath& path);

} // namespace aa::ipc
