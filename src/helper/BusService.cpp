// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusService.hpp>
#include "../ipc/dbus_adapter/BusConnection.hpp"
#include <algorithm>
#include <poll.h>

namespace aa::helper {
namespace {
using namespace ipc::dbus_adapter;
using Clock = std::chrono::steady_clock;
class BusIdentity final : public IdentityLookup {
public:
    BusIdentity(BusConnection connection, IdentityPolicy policy)
        : connection_(std::move(connection)), policy_(std::move(policy)) {}
    core::Result<ReceiverIdentity> resolve(const std::string& sender) override {
        const auto until = Clock::now() + std::chrono::milliseconds(500);
        connection_.deadline(until);
        const ipc::BusName name{sender};
        const auto uid = connection_.get_connection_unix_uid(name);
        const auto pid = connection_.get_connection_unix_pid(name);
        if (!uid || !pid) return Error{ErrorCode::helper_rejected};
        return inspect_process(sender, uid.value(), pid.value(), policy_, until);
    }
private:
    BusConnection connection_;
    IdentityPolicy policy_;
};
bool valid_action(std::uint32_t value) {
    return value >= static_cast<std::uint32_t>(Action::acquire_ap) &&
           value <= static_cast<std::uint32_t>(Action::close_pairing);
}
}
struct BusService::Impl {
    BusConnection export_bus;
    BusIdentity identities;
    SystemHelper helper;
    Clock::time_point next_tick{Clock::now()};
    Impl(BusConnection bus, BusConnection credentials, IdentityPolicy policy, Backend& backend)
        : export_bus(std::move(bus)), identities(std::move(credentials), std::move(policy)),
          helper(identities, backend) {}
    core::Result<void> dispatch(const ParsedMessage& message) {
        if (message.type != MessageType::method_call) return {};
        MessageWriter reply;
        reply.destination(message.sender).reply_serial(message.serial);
        core::Result<std::uint64_t> result{Error{ErrorCode::helper_protocol_mismatch}};
        Cursor body{message.body};
        std::optional<ObservedPeer> peer;
        const bool polling = message.member == "PollPeerV1";
        if (message.path == kObjectPath && message.interface_name == kInterface &&
            !message.sender.empty() && message.sender.front() == ':') {
            if (polling && message.signature.empty() && body.done()) {
                const auto observed = helper.observe(message.sender);
                if (observed) { peer = observed.value(); result = peer->handle; }
                else result = observed.error();
            } else if (message.member == "BindPeerV1" && message.signature == "ttt") {
                const auto generation = body.read_u64();
                const auto confirmation = body.read_u64();
                const auto handle = body.read_u64();
                if (generation && confirmation && handle && body.done()) {
                    const auto bound = helper.bind(message.sender, {generation.value(), confirmation.value()}, handle.value());
                    result = bound ? core::Result<std::uint64_t>{0} : core::Result<std::uint64_t>{bound.error()};
                }
            } else if (message.member == "ReconnectPeerV1" && message.signature == "t") {
                const auto handle = body.read_u64();
                if (handle && body.done()) {
                    const auto connected = helper.reconnect_peer(message.sender, handle.value());
                    result = connected ? core::Result<std::uint64_t>{static_cast<std::uint64_t>(connected.value())}
                                       : core::Result<std::uint64_t>{connected.error()};
                }
            } else if (message.member == "RevokePeerV1" && message.signature == "ss") {
                const auto address = body.read_string();
                const auto adapter = body.read_string();
                if (address && adapter && body.done()) {
                    const auto revoked = helper.revoke_peer(message.sender, {address.value(), adapter.value()});
                    result = revoked ? core::Result<std::uint64_t>{0} : core::Result<std::uint64_t>{revoked.error()};
                }
            } else if (message.member == "Issue" && message.signature == "ttuu") {
                const auto generation = body.read_u64();
                const auto confirmation = body.read_u64();
                const auto action = body.read_u32();
                const auto channel = body.read_u32();
                if (generation && confirmation && action && channel && body.done() && valid_action(action.value()))
                    result = helper.issue(message.sender, {generation.value(), confirmation.value()},
                                          static_cast<Action>(action.value()), channel.value(), Clock::now());
            } else if (message.member == "Execute" && message.signature == "uut") {
                const auto action = body.read_u32();
                const auto channel = body.read_u32();
                const auto grant = body.read_u64();
                if (action && channel && grant && body.done() && valid_action(action.value())) {
                    const auto effect = helper.execute(message.sender, {static_cast<Action>(action.value()),
                                                       channel.value(), grant.value()}, Clock::now());
                    result = effect ? core::Result<std::uint64_t>{static_cast<std::uint64_t>(effect.value())}
                                    : core::Result<std::uint64_t>{effect.error()};
                }
            } else if (message.member == "RevokeAP1" && message.signature == "t") {
                const auto phone = body.read_u64();
                if (phone && body.done()) {
                    const auto revoked = helper.revoke(message.sender, phone.value());
                    result = revoked ? core::Result<std::uint64_t>{0} : core::Result<std::uint64_t>{revoked.error()};
                }
            } else if (message.member == "Invalidate" && message.signature == "tt") {
                const auto generation = body.read_u64();
                const auto confirmation = body.read_u64();
                if (generation && confirmation && body.done()) {
                    const auto invalidated = helper.invalidate(message.sender, {generation.value(), confirmation.value()});
                    result = invalidated ? core::Result<std::uint64_t>{0} : core::Result<std::uint64_t>{invalidated.error()};
                }
            }
        }
        // Every reply has the same typed error/value shape; no dynamic identifiers.
        reply.body_signature(polling ? "utss" : "ut").put_u32(result ? 0U : static_cast<std::uint32_t>(result.error().code()))
              .put_u64(result ? result.value() : 0);
        if (polling) reply.put_string(peer ? peer->identity.address : "").put_string(peer ? peer->identity.adapter : "");
        export_bus.deadline(Clock::now() + std::chrono::milliseconds(500));
        const auto sent = export_bus.send_message(MessageType::method_return, reply);
        return sent ? core::Result<void>{} : core::Result<void>{sent.error()};
    }
};
BusService::BusService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BusService::~BusService() { if (impl_) impl_->helper.bus_lost(); }
BusService::BusService(BusService&&) noexcept = default;
BusService& BusService::operator=(BusService&& other) noexcept {
    if (this != &other) {
        if (impl_) impl_->helper.bus_lost();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
core::Result<BusService> BusService::system(IdentityPolicy policy, Backend& backend) {
    return private_bus("unix:path=/run/dbus/system_bus_socket", std::move(policy), backend);
}
core::Result<BusService> BusService::private_bus(const std::string& address,
                                               IdentityPolicy policy, Backend& backend) {
    auto bus = BusConnection::connect_private(address);
    auto credentials = BusConnection::connect_private(address);
    if (!bus || !credentials) return Error{ErrorCode::helper_unavailable};
    if (!bus.value().request_name(ipc::BusName{kBusName})) return Error{ErrorCode::helper_unavailable};
    return BusService{std::make_unique<Impl>(std::move(bus.value()), std::move(credentials.value()),
                                           std::move(policy), backend)};
}
core::Result<void> BusService::cycle() {
    impl_->helper.platform_tick();
    if (Clock::now() >= impl_->next_tick) {
        impl_->helper.tick(Clock::now());
        impl_->next_tick = Clock::now() + std::chrono::seconds(1);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(impl_->next_tick - Clock::now()).count();
    ::pollfd fd{impl_->export_bus.native_handle(), POLLIN, 0};
    const int ready = ::poll(&fd, 1, static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 20)));
    if (ready < 0 && errno == EINTR) return {};
    if (ready < 0 || (ready > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
        impl_->helper.bus_lost();
        return Error{ErrorCode::helper_unavailable};
    }
    if (!ready) return {};
    impl_->export_bus.deadline(Clock::now() + std::chrono::milliseconds(500));
    const auto message = impl_->export_bus.read_message();
    if (!message) {
        impl_->helper.bus_lost();
        return message.error();
    }
    const auto result = impl_->dispatch(message.value());
    if (!result) impl_->helper.bus_lost();
    return result;
}
}
