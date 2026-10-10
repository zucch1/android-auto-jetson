// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlBusClient.hpp>

#include <aa/ipc/Contract.hpp>

#include "BusConnection.hpp"
#include "BusWire.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <poll.h>

namespace aa::ipc {

namespace {

using dbus_adapter::BusConnection;
using dbus_adapter::Cursor;
using dbus_adapter::MessageWriter;
using dbus_adapter::MessageType;
using dbus_adapter::ParsedMessage;

constexpr int kMaxSkippedMessages = 32;

[[nodiscard]] ErrorCode error_code_for(std::string_view error_name) {
    if (error_name == "org.custom.AndroidAutoReceiver1.Error.PeerUnauthorized") {
        return ErrorCode::ipc_peer_unauthorized;
    }
    if (error_name == "org.custom.AndroidAutoReceiver1.Error.ConsumerRejected") {
        return ErrorCode::ipc_consumer_rejected;
    }
    if (error_name == "org.custom.AndroidAutoReceiver1.Error.QueueFull") {
        return ErrorCode::ipc_queue_full;
    }
    if (error_name == "org.custom.AndroidAutoReceiver1.Error.IllegalTransition") {
        return ErrorCode::session_illegal_transition;
    }
    if (error_name == "org.custom.AndroidAutoReceiver1.Error.MalformedRequest" ||
        error_name == "org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor" ||
        error_name == "org.freedesktop.DBus.Error.InvalidArgs" ||
        error_name == "org.freedesktop.DBus.Error.UnknownMethod" ||
        error_name == "org.freedesktop.DBus.Error.UnknownInterface" ||
        error_name == "org.freedesktop.DBus.Error.UnknownObject") {
        return ErrorCode::ipc_malformed_request;
    }
    return ErrorCode::ipc_malformed_request;
}

[[nodiscard]] bool is_unique_name(std::string_view name) {
    return name.size() >= 2 && name.front() == ':';
}

} // namespace

struct ControlBusClient::Impl final {
    std::optional<BusConnection> connection{};
    std::deque<ContractSignal> pending{};
    std::optional<BusName> owner_cache{};
    std::optional<BusName> consumer_name{};

    [[nodiscard]] core::Result<BusName> resolve_owner() {
        auto owner = connection->get_name_owner(BusName{std::string{kContractBusName}});
        if (!owner.has_value()) {
            owner_cache.reset();
            return owner.error();
        }
        owner_cache = owner.value();
        return owner.value();
    }

    void park_signal(const ParsedMessage& message) {
        if (!is_unique_name(message.sender) || message.interface_name != kContractInterface) {
            return;
        }
        if (owner_cache.has_value() && message.sender != owner_cache->value) {
            return;
        }
        ContractSignal signal{};
        signal.member = message.member;
        Cursor cursor(message.body);
        if (message.member == "StateChanged") {
            auto projection = cursor.read_string();
            auto consumer = cursor.read_u64();
            if (!projection.has_value() || !consumer.has_value() || !cursor.done()) {
                return;
            }
            signal.projection = projection.value() == "projecting" ? ProjectionState::projecting
                                                                   : ProjectionState::stopped;
            signal.consumer = core::ConsumerId{consumer.value()};
        } else if (message.member == "PairingRequest" || message.member == "PairingOpened" ||
                   message.member == "PairingClosed") {
            auto request = cursor.read_u64();
            if (!request.has_value() || !cursor.done()) {
                return;
            }
            signal.request = core::PairingRequestId{request.value()};
        } else if (message.member == "DiagnosticEvent") {
            auto severity = cursor.read_string();
            auto event = cursor.read_string();
            auto subject = cursor.read_u64();
            if (!severity.has_value() || !event.has_value() || !subject.has_value() ||
                !cursor.done()) {
                return;
            }
            if (severity.value() == "info") {
                signal.severity = DiagnosticSeverity::info;
            } else if (severity.value() == "warning") {
                signal.severity = DiagnosticSeverity::warning;
            } else if (severity.value() == "error") {
                signal.severity = DiagnosticSeverity::error;
            } else {
                return;
            }
            signal.event = std::move(event.value());
            signal.subject = subject.value();
        } else {
            return;
        }
        pending.push_back(std::move(signal));
    }

    [[nodiscard]] core::Result<ParsedMessage> exchange(std::string_view member,
                                                       std::string_view signature,
                                                       const MessageWriter& body) {
        const auto owner = owner_cache.has_value()
                               ? core::Result<BusName>{*owner_cache}
                               : resolve_owner();
        if (!owner.has_value()) {
            return owner.error();
        }
        MessageWriter writer = body;
        writer.destination(owner.value().value)
            .path(kContractObjectPath)
            .interface_name(kContractInterface)
            .member(member)
            .body_signature(signature);
        const auto serial = connection->send_message(MessageType::method_call, writer);
        if (!serial.has_value()) {
            return serial.error();
        }
        bool saw_forged = false;
        for (int skipped = 0; skipped < kMaxSkippedMessages; ++skipped) {
            auto message = connection->read_message();
            if (!message.has_value()) {
                return saw_forged ? core::Result<ParsedMessage>{Error{
                                        ErrorCode::transport_unauthorized_peer}}
                                  : core::Result<ParsedMessage>{message.error()};
            }
            if (message.value().type == MessageType::signal) {
                park_signal(message.value());
                continue;
            }
            const bool is_reply = message.value().type == MessageType::method_return ||
                                  message.value().type == MessageType::error_reply;
            if (!is_reply || message.value().reply_serial != serial.value()) {
                continue;
            }
            const bool from_expected = message.value().sender == owner.value().value;
            const bool to_us = message.value().destination == connection->unique_name().value;
            if (!from_expected || !to_us) {
                saw_forged = true;
                continue;
            }
            return message;
        }
        return Error{ErrorCode::transport_io};
    }

    [[nodiscard]] core::Result<void> exchange_void(std::string_view member,
                                                   std::string_view signature,
                                                   const MessageWriter& body) {
        auto reply = exchange(member, signature, body);
        if (!reply.has_value()) {
            return reply.error();
        }
        if (reply.value().type == MessageType::error_reply) {
            return Error{error_code_for(reply.value().error_name)};
        }
        Cursor cursor(reply.value().body);
        return cursor.done() ? core::Result<void>{} : core::Result<void>{Error{ErrorCode::ipc_malformed_request}};
    }
};

core::Result<ControlBusClient> ControlBusClient::connect() {
    auto connection = BusConnection::connect_session();
    if (!connection.has_value()) {
        return connection.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->connection = std::move(connection.value());
    const auto matched = impl->connection->add_match(
        std::string_view{"type='signal',interface='org.custom.AndroidAutoReceiver1'"});
    if (!matched.has_value()) {
        return matched.error();
    }
    return ControlBusClient(std::move(impl));
}

ControlBusClient::ControlBusClient(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ControlBusClient::~ControlBusClient() = default;

ControlBusClient::ControlBusClient(ControlBusClient&& other) noexcept = default;
ControlBusClient& ControlBusClient::operator=(ControlBusClient&& other) noexcept = default;

core::Result<std::optional<BusName>> ControlBusClient::receiver_owner() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    auto owner = impl_->connection->get_name_owner(BusName{std::string{kContractBusName}});
    if (!owner.has_value()) {
        impl_->owner_cache.reset();
        return std::optional<BusName>{};
    }
    impl_->owner_cache = owner.value();
    return std::optional<BusName>{owner.value()};
}

core::Result<core::ConsumerId> ControlBusClient::register_consumer(const BusName& consumer_name) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    const auto owned = impl_->connection->request_name(consumer_name);
    if (!owned.has_value()) {
        return owned.error();
    }
    MessageWriter body;
    body.put_string(consumer_name.value);
    auto reply = impl_->exchange("RegisterConsumer", "s", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    Cursor cursor(reply.value().body);
    auto id = cursor.read_u64();
    if (!id.has_value() || !cursor.done()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    impl_->consumer_name = consumer_name;
    return core::ConsumerId{id.value()};
}

core::Result<void> ControlBusClient::unregister_consumer() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    return impl_->exchange_void("UnregisterConsumer", "", body);
}

core::Result<void> ControlBusClient::start_projection() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    return impl_->exchange_void("StartProjection", "", body);
}

core::Result<void> ControlBusClient::stop_projection() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    return impl_->exchange_void("StopProjection", "", body);
}

core::Result<void> ControlBusClient::set_display_viewport(Viewport viewport) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    body.put_u32(viewport.width).put_u32(viewport.height).put_u32(viewport.dpi);
    return impl_->exchange_void("SetDisplayViewport", "uuu", body);
}

core::Result<StateSnapshot> ControlBusClient::get_state() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    auto reply = impl_->exchange("GetState", "", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    Cursor cursor(reply.value().body);
    auto projection = cursor.read_string();
    auto consumer = cursor.read_u64();
    if (!projection.has_value() || !consumer.has_value() || !cursor.done()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    StateSnapshot snapshot{};
    snapshot.projection = projection.value() == "projecting" ? ProjectionState::projecting
                                                             : ProjectionState::stopped;
    snapshot.consumer = core::ConsumerId{consumer.value()};
    return snapshot;
}

core::Result<std::uint64_t> ControlBusClient::ping() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    auto reply = impl_->exchange("Ping", "", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    Cursor cursor(reply.value().body);
    auto stamp = cursor.read_u64();
    if (!stamp.has_value() || !cursor.done()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    return stamp.value();
}

core::Result<ControlBusClient::ContractSignal> ControlBusClient::wait_signal(
    core::Milliseconds timeout) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    const bool block_forever = timeout.count < 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout.count);
    for (;;) {
        if (!impl_->pending.empty()) {
            auto signal = std::move(impl_->pending.front());
            impl_->pending.pop_front();
            return signal;
        }
        int limit = -1;
        if (!block_forever) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count();
            if (remaining <= 0) {
                return Error{ErrorCode::timeout};
            }
            limit = remaining > 2000 ? 2000 : static_cast<int>(remaining);
        }
        ::pollfd descriptor{impl_->connection->native_handle(), POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, limit);
        if (ready < 0) {
            continue;
        }
        if (ready == 0) {
            if (block_forever) {
                continue;
            }
            return Error{ErrorCode::timeout};
        }
        auto message = impl_->connection->read_message();
        if (!message.has_value()) {
            return message.error();
        }
        if (message.value().type == MessageType::signal) {
            impl_->park_signal(message.value());
        }
    }
}

const BusName& ControlBusClient::unique_name() const noexcept {
    static const BusName empty{};
    return impl_ ? impl_->connection->unique_name() : empty;
}

} // namespace aa::ipc
