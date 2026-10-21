// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlBusService.hpp>

#include <aa/ipc/BusPeerLookup.hpp>
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/ControlDispatcher.hpp>

#include "BusConnection.hpp"
#include "BusWire.hpp"

#include <cerrno>
#include <chrono>
#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include <poll.h>

namespace aa::ipc {

namespace {

using dbus_adapter::BusConnection;
using dbus_adapter::MessageWriter;
using dbus_adapter::MessageType;

// ControlEventSink that marshals every declared event to a REAL D-Bus signal
// on the export connection (the contract's five signals, wire shapes exactly
// as docs/dbus/org.custom.AndroidAutoReceiver1.xml declares them).
class BusSignalEmitter final : public ControlEventSink {
public:
    void bind(BusConnection* connection) noexcept { connection_ = connection; }

    void emit(const ControlEvent& event) override {
        if (connection_ == nullptr) {
            return;
        }
        if (const auto* changed = std::get_if<StateChanged>(&event)) {
            MessageWriter writer;
            writer.path(kContractObjectPath)
                .interface_name(kContractInterface)
                .member("StateChanged")
                .body_signature("st");
            writer.put_string(to_string(changed->projection));
            writer.put_u64(changed->consumer.value);
            static_cast<void>(connection_->send_message(MessageType::signal, writer));
            return;
        }
        std::string_view member{};
        std::uint64_t request{};
        if (const auto* requested = std::get_if<PairingRequested>(&event)) {
            member = "PairingRequest";
            request = requested->request.value;
        } else if (const auto* opened = std::get_if<PairingOpened>(&event)) {
            member = "PairingOpened";
            request = opened->request.value;
        } else if (const auto* closed = std::get_if<PairingClosed>(&event)) {
            member = "PairingClosed";
            request = closed->request.value;
        } else if (const auto* diagnostic = std::get_if<DiagnosticEmitted>(&event)) {
            MessageWriter writer;
            writer.path(kContractObjectPath)
                .interface_name(kContractInterface)
                .member("DiagnosticEvent")
                .body_signature("sst");
            writer.put_string(to_string(diagnostic->severity));
            writer.put_string(diagnostic->event);
            writer.put_u64(diagnostic->subject);
            static_cast<void>(connection_->send_message(MessageType::signal, writer));
            return;
        } else {
            return;
        }
        MessageWriter writer;
        writer.path(kContractObjectPath)
            .interface_name(kContractInterface)
            .member(member)
            .body_signature("t");
        writer.put_u64(request);
        static_cast<void>(connection_->send_message(MessageType::signal, writer));
    }

private:
    BusConnection* connection_{nullptr};
};

} // namespace

struct ControlBusService::Impl final {
    std::optional<BusConnection> connection{};
    BusPeerLookup credentials{};
    BusSignalEmitter emitter{};
    PairingIdSource& pairing_ids;
    PhoneDirectory& phones;
    ControlService service;
    ControlDispatcher dispatcher;
    std::uint64_t idle_refreshes{0};
    bool closed{false};
    std::chrono::steady_clock::time_point next_refresh{std::chrono::steady_clock::now()};

    Impl(ControlServiceConfig config, PairingIdSource& ids, PhoneDirectory& directory,
         helper::Confirmation* confirmation)
        : pairing_ids(ids), phones(directory),
          service(config, ControlServiceDeps{credentials, emitter, pairing_ids, phones, confirmation}),
          dispatcher(service) {}
};

core::Result<ControlBusService> ControlBusService::start(Config config, Deps deps) {
    auto connection = dbus_adapter::BusConnection::connect_session();
    if (!connection.has_value()) {
        return connection.error();
    }
    auto impl = std::make_unique<Impl>(ControlServiceConfig{config.session_unix_uid},
                                       deps.pairing_ids, deps.phones, deps.confirmation);
    impl->connection = std::move(connection.value());
    impl->emitter.bind(&*impl->connection);
    const auto owned =
        impl->connection->request_name(BusName{std::string{kContractBusName}});
    if (!owned.has_value()) {
        return owned.error();
    }
    return ControlBusService(std::move(impl));
}

ControlBusService::ControlBusService(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ControlBusService::~ControlBusService() { close(); }

ControlBusService::ControlBusService(ControlBusService&& other) noexcept = default;
ControlBusService& ControlBusService::operator=(ControlBusService&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

core::Result<std::size_t> ControlBusService::cycle(core::Milliseconds idle_interval) {
    if (!impl_ || impl_->closed || !impl_->connection.has_value()) {
        return Error{ErrorCode::transport_closed};
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= impl_->next_refresh) {
        impl_->service.refresh_consumer_owner();
        impl_->next_refresh = now + std::chrono::seconds(1);
    }
    const int limit = static_cast<int>(idle_interval.count < 0 ? 1000 :
                                       std::min<std::int64_t>(idle_interval.count, 1000));
    ::pollfd descriptor{impl_->connection->native_handle(), POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, limit);
    if (ready < 0) {
        if (errno == EINTR) {
            return std::size_t{0};
        }
        return Error{ErrorCode::transport_io};
    }
    if (ready == 0) {
        impl_->service.refresh_consumer_owner();
        ++impl_->idle_refreshes;
        return std::size_t{0};
    }
    auto message = impl_->connection->read_message();
    if (!message.has_value()) {
        return message.error();
    }
    const auto call = dbus_adapter::to_incoming_call(message.value());
    const auto reply = impl_->dispatcher.handle(call);
    if (!reply.has_value()) {
        return std::size_t{0};
    }
    const auto sent = impl_->connection->send_reply(message.value().serial,
                                                    BusName{message.value().sender}, reply.value());
    if (!sent.has_value()) {
        return sent.error();
    }
    return std::size_t{1};
}

void ControlBusService::close() noexcept {
    if (!impl_ || impl_->closed) {
        return;
    }
    impl_->closed = true;
    impl_->service.shutdown();
    impl_->emitter.bind(nullptr);
    if (impl_->connection.has_value()) {
        static_cast<void>(
            impl_->connection->release_name(BusName{std::string{kContractBusName}}));
        impl_->connection.reset();
    }
}

bool ControlBusService::closed() const noexcept {
    return !impl_ || impl_->closed;
}

const BusName& ControlBusService::unique_name() const noexcept {
    static const BusName empty{};
    if (!impl_ || !impl_->connection.has_value()) {
        return empty;
    }
    return impl_->connection->unique_name();
}

std::uint64_t ControlBusService::idle_refreshes() const noexcept {
    return impl_ ? impl_->idle_refreshes : 0;
}

ControlService& ControlBusService::service() noexcept {
    return impl_->service;
}

} // namespace aa::ipc
