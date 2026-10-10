// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusConnection.hpp"

#include "BusIo.hpp"

#include <string_view>

namespace aa::ipc::dbus_adapter {

namespace {

constexpr int kMaxSkippedMessages = 32;
constexpr std::uint32_t kRequestNameOwned = 1;
constexpr std::uint32_t kRequestNameAlreadyOwner = 4;
constexpr std::string_view kDaemonName = "org.freedesktop.DBus";
constexpr std::string_view kDaemonPath = "/org/freedesktop/DBus";

} // namespace

core::Result<std::uint32_t> BusConnection::send_message(MessageType type,
                                                        const MessageWriter& message) {
    const std::uint32_t serial = next_serial_++;
    const auto sent = bus_io::write_full(fd_, message.build(type, serial));
    if (!sent.has_value()) {
        return sent.error();
    }
    return serial;
}

core::Result<ParsedMessage> BusConnection::await_reply(std::uint32_t serial,
                                                       const BusName& expected_sender) {
    bool saw_forged = false;
    for (int skipped = 0; skipped < kMaxSkippedMessages; ++skipped) {
        auto message = read_message();
        if (!message.has_value()) {
            return saw_forged ? core::Result<ParsedMessage>{Error{ErrorCode::transport_unauthorized_peer}}
                              : core::Result<ParsedMessage>{message.error()};
        }
        const bool is_reply = message.value().type == MessageType::method_return ||
                              message.value().type == MessageType::error_reply;
        if (!is_reply || message.value().reply_serial != serial) {
            continue;
        }
        const bool from_expected = message.value().sender == expected_sender.value;
        const bool to_us = unique_name_.value.empty()
                               ? !message.value().destination.empty() &&
                                     message.value().destination.front() == ':'
                               : message.value().destination == unique_name_.value;
        if (!from_expected || !to_us) {
            saw_forged = true;
            continue;
        }
        return message;
    }
    return Error{saw_forged ? ErrorCode::transport_unauthorized_peer : ErrorCode::transport_io};
}

core::Result<void> BusConnection::send_reply(std::uint32_t reply_serial, const BusName& destination,
                                             const OutboundReply& reply) {
    MessageWriter writer;
    writer.reply_serial(reply_serial).destination(destination.value);
    if (reply.is_error) {
        writer.error_name(reply.error_name).body_signature("s");
        const auto* message = std::get_if<std::string>(&reply.body);
        writer.put_string(message != nullptr ? *message : "dispatch error");
        const auto sent = send_message(MessageType::error_reply, writer);
        return sent.has_value() ? core::Result<void>{} : core::Result<void>{sent.error()};
    }
    switch (reply.body.index()) {
    case 1:
        writer.body_signature("t").put_u64(std::get<std::uint64_t>(reply.body));
        break;
    case 2:
        writer.body_signature("s").put_string(std::get<std::string>(reply.body));
        break;
    case 3: {
        const auto& state = std::get<StateSnapshot>(reply.body);
        writer.body_signature("st")
            .put_string(to_string(state.projection))
            .put_u64(state.consumer.value);
        break;
    }
    case 4:
        writer.body_signature("a{ss}").put_string_map(std::get<std::map<std::string, std::string>>(reply.body));
        break;
    default:
        break;
    }
    const auto sent = send_message(MessageType::method_return, writer);
    return sent.has_value() ? core::Result<void>{} : core::Result<void>{sent.error()};
}

core::Result<ParsedMessage> BusConnection::call(std::string_view member,
                                                std::string_view body_signature,
                                                MessageWriter body) {
    body.destination(kDaemonName)
        .path(kDaemonPath)
        .interface_name(kDaemonName)
        .member(member)
        .body_signature(body_signature);
    const auto sent = send_message(MessageType::method_call, body);
    if (!sent.has_value()) {
        return sent.error();
    }
    return await_reply(sent.value(), BusName{std::string{kDaemonName}});
}

core::Result<void> BusConnection::request_name(const BusName& name) {
    MessageWriter body;
    body.put_string(name.value).put_u32(0);
    const auto reply = call("RequestName", "su", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{ErrorCode::transport_io};
    }
    const auto outcome = reply_u32(reply.value());
    if (!outcome.has_value()) {
        return outcome.error();
    }
    if (outcome.value() != kRequestNameOwned && outcome.value() != kRequestNameAlreadyOwner) {
        return Error{ErrorCode::transport_unavailable};
    }
    return {};
}

core::Result<void> BusConnection::release_name(const BusName& name) {
    MessageWriter body;
    body.put_string(name.value);
    const auto reply = call("ReleaseName", "s", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{ErrorCode::transport_io};
    }
    const auto outcome = reply_u32(reply.value());
    return outcome.has_value() ? core::Result<void>{} : core::Result<void>{outcome.error()};
}

core::Result<BusName> BusConnection::get_name_owner(const BusName& name) {
    MessageWriter body;
    body.put_string(name.value);
    const auto reply = call("GetNameOwner", "s", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    auto owner = reply_string(reply.value());
    if (!owner.has_value()) {
        return owner.error();
    }
    return BusName{owner.value()};
}

core::Result<std::uint32_t> BusConnection::get_connection_unix_uid(const BusName& name) {
    MessageWriter body;
    body.put_string(name.value);
    const auto reply = call("GetConnectionUnixUser", "s", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    return reply_u32(reply.value());
}

aa::ipc::IncomingCall to_incoming_call(const ParsedMessage& message) {
    aa::ipc::WireType type = aa::ipc::WireType::other;
    switch (message.type) {
    case MessageType::method_call: type = aa::ipc::WireType::method_call; break;
    case MessageType::method_return: type = aa::ipc::WireType::method_return; break;
    case MessageType::error_reply: type = aa::ipc::WireType::error_reply; break;
    case MessageType::signal: type = aa::ipc::WireType::signal; break;
    }
    return aa::ipc::IncomingCall{BusName{message.sender}, type, message.path,
                                 message.interface_name, message.member, message.signature,
                                 message.body};
}

} // namespace aa::ipc::dbus_adapter
