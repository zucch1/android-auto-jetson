// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/BusPeerLookup.hpp>

#include "BusConnection.hpp"

namespace aa::ipc {

struct BusPeerLookup::Impl final {
    explicit Impl(core::Result<dbus_adapter::BusConnection> result) : connection(std::move(result)) {}
    core::Result<dbus_adapter::BusConnection> connection;
};

BusPeerLookup::BusPeerLookup()
    : impl_(std::make_unique<Impl>(dbus_adapter::BusConnection::connect_session())) {}

BusPeerLookup::~BusPeerLookup() = default;

bool BusPeerLookup::connected() const noexcept {
    return impl_ && impl_->connection.has_value();
}

const BusName& BusPeerLookup::unique_name() const {
    static const BusName kDisconnected{};
    if (!connected()) {
        return kDisconnected;
    }
    return impl_->connection.value().unique_name();
}

core::Result<std::uint32_t> BusPeerLookup::unix_uid(const BusName& name) const {
    if (!connected()) {
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    const auto uid = impl_->connection.value().get_connection_unix_uid(name);
    if (!uid.has_value()) {
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    return uid.value();
}

core::Result<BusName> BusPeerLookup::name_owner(const BusName& name) const {
    if (!connected()) {
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    const auto owner = impl_->connection.value().get_name_owner(name);
    if (!owner.has_value()) {
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    return owner.value();
}

} // namespace aa::ipc
