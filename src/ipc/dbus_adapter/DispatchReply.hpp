// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reply shaping and typed error names shared by the dispatch translation
// units (contract-method dispatch and the public Properties surface). Private
// to src/ipc/dbus_adapter; the wire adapter marshals these OutboundReply
// values.

#include <aa/ipc/ControlDispatcher.hpp>

#include "BusWire.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace aa::ipc {

inline constexpr std::string_view kContractNamePrefix = "org.custom.AndroidAutoReceiver";
inline constexpr std::string_view kUnsupportedMajorError =
    "org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor";
inline constexpr std::string_view kPeerUnauthorizedError =
    "org.custom.AndroidAutoReceiver1.Error.PeerUnauthorized";
inline constexpr std::string_view kConsumerRejectedError =
    "org.custom.AndroidAutoReceiver1.Error.ConsumerRejected";
inline constexpr std::string_view kMalformedRequestError =
    "org.custom.AndroidAutoReceiver1.Error.MalformedRequest";
inline constexpr std::string_view kQueueFullError = "org.custom.AndroidAutoReceiver1.Error.QueueFull";
inline constexpr std::string_view kIllegalTransitionError =
    "org.custom.AndroidAutoReceiver1.Error.IllegalTransition";

[[nodiscard]] inline OutboundReply error_reply(std::string_view name, std::string_view message) {
    return OutboundReply{true, std::string{name}, std::string{message}};
}

// Builds a typed reply with the alternative constructed in place (never via
// the variant converting constructor): that constructor path makes GCC 13 -O3
// report false maybe-uninitialized diagnostics on std::variant storage.
template <typename T>
[[nodiscard]] inline OutboundReply ok(T body) {
    return OutboundReply{false, {}, ReplyBody{std::in_place_type<T>, std::move(body)}};
}

[[nodiscard]] inline bool body_consumed(const dbus_adapter::Cursor& cursor) {
    return cursor.done();
}

} // namespace aa::ipc
