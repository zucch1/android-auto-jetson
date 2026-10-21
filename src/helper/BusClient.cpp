// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include "../ipc/dbus_adapter/BusConnection.hpp"
#include <algorithm>
#include <sys/socket.h>

namespace aa::helper {
namespace {
using namespace ipc::dbus_adapter;
using Clock = std::chrono::steady_clock;
}
struct BusClient::Impl {
    BusConnection bus;
    trust::ApprovedPhoneStore& store;
    struct Pending { Source source; Action action; std::uint64_t grant; };
    std::vector<Pending> pending;
    std::optional<Source> active_source;
    Impl(BusConnection connection, trust::ApprovedPhoneStore& approvals)
        : bus(std::move(connection)), store(approvals) {}
    core::Result<std::uint64_t> rpc(const char* member, const char* signature, MessageWriter writer,
                                    Clock::time_point until) {
        bus.deadline(until);
        const auto owner = bus.get_name_owner(ipc::BusName{kBusName});
        if (!owner) return Error{Clock::now() >= until ? ErrorCode::timeout : ErrorCode::helper_unavailable};
        writer.destination(owner.value().value).path(kObjectPath).interface_name(kInterface)
              .member(member).body_signature(signature);
        const auto serial = bus.send_message(MessageType::method_call, writer);
        if (!serial) return Error{Clock::now() >= until ? ErrorCode::timeout : ErrorCode::helper_unavailable};
        const auto reply = bus.await_reply(serial.value(), owner.value());
        if (Clock::now() >= until) return Error{ErrorCode::timeout};
        if (!reply) return Error{ErrorCode::helper_unavailable};
        if (reply.value().type != MessageType::method_return || reply.value().signature != "ut")
            return Error{ErrorCode::helper_protocol_mismatch};
        Cursor body{reply.value().body};
        const auto error = body.read_u32();
        const auto value = body.read_u64();
        if (!error || !value || !body.done()) return Error{ErrorCode::helper_protocol_mismatch};
        switch (error.value()) {
        case 0: return value.value();
        case 1000: return Error{ErrorCode::helper_unavailable};
        case 1001: return Error{ErrorCode::helper_rejected};
        default: return Error{ErrorCode::helper_protocol_mismatch};
        }
    }
    core::Result<State> execute(Invocation invocation, Clock::time_point until) {
        const auto [action, channel, grant] = invocation;
        MessageWriter writer;
        writer.put_u32(static_cast<std::uint32_t>(action)).put_u32(channel).put_u64(grant);
        const auto result = rpc("Execute", "uut", writer, until);
        if (!result) return result.error();
        if (result.value() > static_cast<std::uint64_t>(State::unavailable))
            return Error{ErrorCode::helper_protocol_mismatch};
        return static_cast<State>(result.value());
    }
    void invalidate(Source source) {
        if (active_source && (!source.generation || source.generation == active_source->generation) &&
            (!source.confirmation || source.confirmation == active_source->confirmation)) active_source.reset();
        std::erase_if(pending, [source](const Pending& p) {
            return (!source.generation || p.source.generation == source.generation) &&
                   (!source.confirmation || p.source.confirmation == source.confirmation);
        });
        MessageWriter writer;
        writer.put_u64(source.generation).put_u64(source.confirmation);
        (void)rpc("Invalidate", "tt", writer, Clock::now() + std::chrono::milliseconds(500));
    }
};
BusClient::BusClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BusClient::~BusClient() = default;
core::Result<std::unique_ptr<BusClient>> BusClient::system(trust::ApprovedPhoneStore& store) {
    return private_bus("unix:path=/run/dbus/system_bus_socket", store);
}
core::Result<std::unique_ptr<BusClient>> BusClient::private_bus(const std::string& address,
                                                            trust::ApprovedPhoneStore& store) {
    auto connection = BusConnection::connect_private(address);
    if (!connection) return Error{ErrorCode::helper_unavailable};
    return std::unique_ptr<BusClient>{new BusClient{std::make_unique<Impl>(std::move(connection.value()), store)}};
}
core::Result<void> BusClient::confirmed(core::ConsumerId generation, core::PairingRequestId source) {
    if (impl_->pending.size() + 2 > SystemHelper::kMaxPending) return Error{ErrorCode::helper_rejected};
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    for (const auto action : {Action::acquire_ap, Action::open_pairing}) {
        MessageWriter writer;
        writer.put_u64(generation.value).put_u64(source.value)
              .put_u32(static_cast<std::uint32_t>(action)).put_u32(action == Action::acquire_ap ? 36U : 0U);
        const auto grant = impl_->rpc("Issue", "ttuu", writer, until);
        if (!grant || !grant.value()) {
            impl_->invalidate({generation.value, source.value});
            return grant ? Error{ErrorCode::helper_protocol_mismatch} : grant.error();
        }
        impl_->pending.push_back({{generation.value, source.value}, action, grant.value()});
    }
    impl_->active_source = Source{generation.value, source.value};
    return {};
}
void BusClient::cancel(core::ConsumerId generation, core::PairingRequestId source) {
    impl_->invalidate({generation.value, source.value});
}
void BusClient::teardown(core::ConsumerId generation) { impl_->invalidate({generation.value, 0}); }
void BusClient::forgotten() { impl_->invalidate({0, 0}); }
core::Result<State> BusClient::status(Action action) {
    if (action != Action::ap_status && action != Action::usb_status && action != Action::modem_status)
        return Error{ErrorCode::invalid_argument};
    return impl_->execute({action, 0, 0}, Clock::now() + std::chrono::milliseconds(1000));
}
core::Result<State> BusClient::reconnect(const trust::TransportIdentity& identity) {
    const auto key = trust::validated_key(identity);
    if (!key || impl_->store.lookup(trust::PhoneIdentity{key.value()}) != trust::Decision::approved)
        return Error{ErrorCode::trust_unknown_phone};
    return impl_->execute({Action::reconnect, 0, 0}, Clock::now() + std::chrono::milliseconds(1000));
}
core::Result<Response> BusClient::call(const Request& request, core::Milliseconds timeout,
                                      core::CancellationToken cancellation) {
    if (cancellation.stop_requested()) return Error{ErrorCode::cancelled};
    if (timeout.count <= 0) return Error{ErrorCode::timeout};
    std::stop_callback interrupt{cancellation, [fd = impl_->bus.native_handle()] {
        (void)::shutdown(fd, SHUT_RDWR);
    }};
    Action action{};
    switch (request.op) {
    case Operation::ensure_access_point: action = Action::acquire_ap; break;
    case Operation::teardown_access_point: action = Action::release_ap; break;
    case Operation::register_bluez_profile: action = Action::register_profile; break;
    case Operation::unregister_bluez_profile: action = Action::unregister_profile; break;
    case Operation::open_pairing_window: action = Action::open_pairing; break;
    case Operation::apply_usb_policy:
    case Operation::exclude_modem_manager: return Error{ErrorCode::helper_unavailable};
    }
    std::uint64_t grant{};
    if (action == Action::acquire_ap || action == Action::open_pairing) {
        const Source explicit_source{request.consumer_generation.value, request.confirmation_source.value};
        const auto source = explicit_source.generation || explicit_source.confirmation ?
                            std::optional<Source>{explicit_source} : impl_->active_source;
        if (!source || !source->generation || !source->confirmation)
            return Error{ErrorCode::helper_rejected};
        const auto found = std::find_if(impl_->pending.begin(), impl_->pending.end(),
                                       [action, source](const Impl::Pending& p) {
                                           return p.action == action && p.source == *source;
                                       });
        if (found == impl_->pending.end()) return Error{ErrorCode::helper_rejected};
        grant = found->grant;
        impl_->pending.erase(found);
    }
    const auto until = Clock::now() + std::chrono::milliseconds(std::min<std::int64_t>(timeout.count, 1000));
    const auto result = impl_->execute({action, action == Action::acquire_ap ? request.channel : 0U, grant}, until);
    if (cancellation.stop_requested()) return Error{ErrorCode::cancelled};
    if (!result) return result.error();
    return Response{result.value() != State::unavailable};
}
}
