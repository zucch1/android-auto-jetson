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
    if (error_name == "org.custom.AndroidAutoReceiver1.Error.NotWiredCandidate") {
        return ErrorCode::trust_not_wired_candidate;
    }
    if (error_name == "org.custom.AndroidAutoReceiver1.Error.NotDurable") {
        return ErrorCode::trust_store_not_durable;
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

[[nodiscard]] core::Result<std::map<std::string, std::string>> parse_string_map(
    Cursor& cursor, std::size_t end) {
    if (!cursor.align(8)) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    std::map<std::string, std::string> entries;
    while (cursor.position() < end) {
        if (!cursor.align(8)) {
            return Error{ErrorCode::ipc_malformed_request};
        }
        auto key = cursor.read_string();
        auto value = cursor.read_string();
        if (!key.has_value() || !value.has_value() || entries.size() >= 16) {
            return Error{ErrorCode::ipc_malformed_request};
        }
        entries.insert_or_assign(std::move(key.value()), std::move(value.value()));
    }
    if (cursor.position() != end) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    return entries;
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
        } else if (message.member == "PairingCompleted") {
            auto request = cursor.read_u64();
            auto outcome = cursor.read_string();
            if (!request.has_value() || !outcome.has_value() || !cursor.done()) {
                return;
            }
            signal.request = core::PairingRequestId{request.value()};
            signal.outcome = std::move(outcome.value());
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
                if (message.error().code() == ErrorCode::transport_message_dropped) {
                    continue;
                }
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

core::Result<ControlBusClient> ControlBusClient::connect(Signals signals) {
    auto connection = BusConnection::connect_session();
    if (!connection.has_value()) {
        return connection.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->connection = std::move(connection.value());
    if (signals == Signals::subscribe) {
        const auto matched = impl->connection->add_match(
            std::string_view{"type='signal',interface='org.custom.AndroidAutoReceiver1'"});
        if (!matched.has_value()) {
            return matched.error();
        }
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
    auto owner = impl_->connection->find_name_owner(BusName{std::string{kContractBusName}});
    if (owner) {
        impl_->owner_cache = owner.value();
    }
    return owner;
}

bool ControlBusClient::disconnected() const noexcept {
    const int fd = impl_ ? impl_->connection->native_handle() : -1;
    if (fd < 0) {
        return true;
    }
    ::pollfd socket{fd, 0, 0};
    return ::poll(&socket, 1, 0) > 0 && (socket.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

core::Result<core::ConsumerId> ControlBusClient::register_consumer(const BusName& consumer_name) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    if (impl_->consumer_name.has_value()) {
        // A populated registration is never replaced in place: the duplicate
        // request is rejected BEFORE another name is requested and the
        // existing name stays owned and tracked.
        return Error{ErrorCode::session_illegal_transition};
    }
    const auto owned = impl_->connection->request_name(consumer_name);
    if (!owned.has_value()) {
        return owned.error();
    }
    // ONE cleanup helper: after the name acquisition EVERY failure (exchange
    // / owner-resolution / transport failure, receiver error reply, malformed
    // registration reply including trailing bytes) best-effort releases the
    // just-acquired name so no lingering ownership outlives the failed
    // registration - the ORIGINAL registration error is what the caller sees.
    const auto unwind = [&](Error error) -> core::Result<core::ConsumerId> {
        static_cast<void>(impl_->connection->release_name(consumer_name));
        return error;
    };
    MessageWriter body;
    body.put_string(consumer_name.value);
    auto reply = impl_->exchange("RegisterConsumer", "s", body);
    if (!reply.has_value()) {
        return unwind(reply.error());
    }
    if (reply.value().type == MessageType::error_reply) {
        return unwind(Error{error_code_for(reply.value().error_name)});
    }
    Cursor cursor(reply.value().body);
    auto id = cursor.read_u64();
    if (!id.has_value() || !cursor.done()) {
        return unwind(Error{ErrorCode::ipc_malformed_request});
    }
    impl_->consumer_name = consumer_name;
    return core::ConsumerId{id.value()};
}

core::Result<void> ControlBusClient::unregister_consumer() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    auto unregistered = impl_->exchange_void("UnregisterConsumer", "", body);
    if (!unregistered.has_value()) {
        // Failure retains the tracked name (and its bus ownership): a retry
        // stays possible and a duplicate register stays rejected.
        return unregistered.error();
    }
    // Success releases the well-known name and resets the tracking so a
    // fresh registration is allowed.
    if (impl_->consumer_name.has_value()) {
        static_cast<void>(impl_->connection->release_name(*impl_->consumer_name));
        impl_->consumer_name.reset();
    }
    return {};
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

core::Result<void> ControlBusClient::request_idr_recovery() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    return impl_->exchange_void("RequestIdrRecovery", "", body);
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

core::Result<std::map<std::string, std::string>> ControlBusClient::get_capabilities() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    auto reply = impl_->exchange("GetCapabilities", "", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    Cursor cursor(reply.value().body);
    const auto length = cursor.read_u32();
    if (!length.has_value() || length.value() > 4096 || !cursor.align(8)) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    const std::size_t end = cursor.position() + length.value();
    if (end > reply.value().body.size()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    auto entries = parse_string_map(cursor, end);
    if (!entries.has_value() || !cursor.done()) {
        return entries.has_value() ? core::Result<std::map<std::string, std::string>>{
                                         Error{ErrorCode::ipc_malformed_request}}
                                   : entries;
    }
    return entries;
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

core::Result<core::PairingRequestId> ControlBusClient::request_add_phone() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    auto reply = impl_->exchange("RequestAddPhone", "", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    Cursor cursor(reply.value().body);
    auto id = cursor.read_u64();
    if (!id.has_value() || !cursor.done() || id.value() == 0) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    return core::PairingRequestId{id.value()};
}

core::Result<std::map<std::string, std::string>> ControlBusClient::pairing_candidate(
    core::PairingRequestId request) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    body.put_u64(request.value);
    auto reply = impl_->exchange("GetPairingCandidate", "t", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    // a{ss}: u32 byte length, pad to 8, then 8-aligned (s, s) dict entries.
    Cursor cursor(reply.value().body);
    const auto length = cursor.read_u32();
    if (!length.has_value() || length.value() > 4096 || !cursor.align(8)) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    const std::size_t end = cursor.position() + length.value();
    if (end > reply.value().body.size()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    auto entries = parse_string_map(cursor, end);
    if (!entries.has_value() || !cursor.done()) {
        return entries.has_value() ? core::Result<std::map<std::string, std::string>>{
                                         Error{ErrorCode::ipc_malformed_request}}
                                   : entries;
    }
    return entries;
}

core::Result<std::vector<std::map<std::string, std::string>>> ControlBusClient::list_phones() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    auto reply = impl_->exchange("ListPhones", "", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    Cursor cursor(reply.value().body);
    const auto outer_length = cursor.read_u32();
    if (!outer_length.has_value() || outer_length.value() > dbus_adapter::kMaxMessageBytes ||
        !cursor.align(4)) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    const std::size_t outer_end = cursor.position() + outer_length.value();
    if (outer_end > reply.value().body.size()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    std::vector<std::map<std::string, std::string>> phones;
    while (cursor.position() < outer_end) {
        if (phones.size() >= 256) {
            return Error{ErrorCode::ipc_malformed_request};
        }
        const auto inner_length = cursor.read_u32();
        if (!inner_length.has_value() || inner_length.value() > 4096 || !cursor.align(8)) {
            return Error{ErrorCode::ipc_malformed_request};
        }
        const std::size_t inner_end = cursor.position() + inner_length.value();
        if (inner_end > outer_end) {
            return Error{ErrorCode::ipc_malformed_request};
        }
        auto entry = parse_string_map(cursor, inner_end);
        if (!entry.has_value()) {
            return entry.error();
        }
        phones.push_back(std::move(entry.value()));
    }
    if (cursor.position() != outer_end || !cursor.done()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    return phones;
}

core::Result<std::string> ControlBusClient::pairing_outcome(core::PairingRequestId request) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    body.put_u64(request.value);
    auto reply = impl_->exchange("GetPairingOutcome", "t", body);
    if (!reply.has_value()) {
        return reply.error();
    }
    if (reply.value().type == MessageType::error_reply) {
        return Error{error_code_for(reply.value().error_name)};
    }
    Cursor cursor(reply.value().body);
    auto outcome = cursor.read_string();
    if (!outcome.has_value() || !cursor.done()) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    return outcome.value();
}

core::Result<void> ControlBusClient::confirm_phone_pairing(core::PairingRequestId request) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    body.put_u64(request.value);
    return impl_->exchange_void("ConfirmPhonePairing", "t", body);
}

core::Result<void> ControlBusClient::cancel_phone_pairing(core::PairingRequestId request) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    body.put_u64(request.value);
    return impl_->exchange_void("CancelPhonePairing", "t", body);
}

core::Result<void> ControlBusClient::forget_phone(core::PhoneId phone) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    MessageWriter body;
    body.put_u64(phone.value);
    return impl_->exchange_void("ForgetPhone", "t", body);
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
            // A zero or expired timeout still polls the socket once without
            // blocking, so a non-blocking caller (the display pump) can drain
            // signals that have arrived; poll() returning 0 ends the wait.
            limit = remaining <= 0 ? 0 : (remaining > 2000 ? 2000 : static_cast<int>(remaining));
        }
        if (impl_->connection->native_handle() < 0) return Error{ErrorCode::transport_closed};
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
            if (message.error().code() == ErrorCode::transport_message_dropped) {
                continue;
            }
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
