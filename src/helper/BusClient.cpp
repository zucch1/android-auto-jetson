// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusClientImpl.hpp"
#include <algorithm>
#include <sys/socket.h>

namespace aa::helper {
BusClient::BusClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BusClient::~BusClient() = default;
core::Result<ObservedPeer> BusClient::poll_bootstrap(Source source, HandoffFailure& failure) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer; writer.put_u64(source.generation).put_u64(source.confirmation);
    ObservedPeer peer;
    const auto result = impl_->rpc("PollBootstrapV1", "tt", writer, until, &peer, &failure);
    if (!result) return result.error();
    if (!peer.handle) return Error{ErrorCode::helper_rejected};
    return peer;
}
core::Result<void> BusClient::authorize_bootstrap(Source source, std::uint64_t handle, bool allow, bool ready) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer; writer.put_u64(source.generation).put_u64(source.confirmation).put_u64(handle)
        .put_u32(allow ? 1U : 0U).put_u32(ready ? 1U : 0U);
    const auto result = impl_->rpc("AuthorizeBootstrapV1", "tttuu", writer, until);
    return result ? core::Result<void>{} : core::Result<void>{result.error()};
}
core::Result<ApSnapshot> BusClient::get_ap_credentials(Source source, std::uint64_t handle) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer; writer.put_u64(source.generation).put_u64(source.confirmation).put_u64(handle);
    ApSnapshot snapshot;
    const auto result = impl_->rpc("GetAPCredentialsV1", "ttt", writer, until, nullptr, nullptr, &snapshot);
    if (!result) return result.error();
    return snapshot;
}
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
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until) || Clock::now() >= until) return Error{ErrorCode::timeout};
    if (impl_->pending.size() + 2 > SystemHelper::kMaxPending) return Error{ErrorCode::helper_rejected};
    for (const auto action : {Action::acquire_ap, Action::open_pairing}) {
        MessageWriter writer;
        writer.put_u64(generation.value).put_u64(source.value)
              .put_u32(static_cast<std::uint32_t>(action)).put_u32(action == Action::acquire_ap ? 36U : 0U);
        const auto grant = impl_->rpc("Issue", "ttuu", writer, until);
        if (!grant || !grant.value()) {
            (void)impl_->invalidate_locked({generation.value, source.value}, until);
            return grant ? Error{ErrorCode::helper_protocol_mismatch} : grant.error();
        }
        impl_->pending.push_back({{generation.value, source.value}, action, grant.value()});
    }
    const auto pairing = impl_->pending.back();
    impl_->pending.pop_back();
    const auto opened = impl_->execute({Action::open_pairing, 0, pairing.grant}, until);
    if (!opened || opened.value() != State::active) {
        (void)impl_->invalidate_locked({generation.value, source.value}, until);
        return opened ? Error{ErrorCode::helper_unavailable} : opened.error();
    }
    impl_->active_source = Source{generation.value, source.value};
    return {};
}
core::Result<State> BusClient::acquire_ap(Source source) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    if (!source.generation || !source.confirmation) return Error{ErrorCode::helper_rejected};
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until) || Clock::now() >= until) return Error{ErrorCode::timeout};
    MessageWriter writer;
    writer.put_u64(source.generation).put_u64(source.confirmation)
        .put_u32(static_cast<std::uint32_t>(Action::acquire_ap)).put_u32(36);
    const auto grant = impl_->rpc("Issue", "ttuu", writer, until);
    if (!grant) return grant.error();
    if (!grant.value()) return Error{ErrorCode::helper_protocol_mismatch};
    const auto result = impl_->execute({Action::acquire_ap, 36, grant.value()}, until);
    if (!result || result.value() == State::unavailable) {
        (void)impl_->invalidate_locked(source, until);
        return result ? Error{ErrorCode::helper_unavailable} : result.error();
    }
    impl_->active_source = source;
    impl_->ap_source = source;
    return result;
}
void BusClient::cancel(core::ConsumerId generation, core::PairingRequestId source) {
    impl_->invalidate({generation.value, source.value});
}
void BusClient::teardown(core::ConsumerId generation) { impl_->invalidate({generation.value, 0}); }
RevocationAttempt BusClient::forgotten_v1(const RevocationV1& snapshot, const RevocationProgress& progress) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    const auto peer = revocation_peer(snapshot);
    RevocationAttempt report;
    if (progress.ap == RevocationLeg::complete) report.ap = {};
    if (peer) report.bond = progress.bond == RevocationLeg::complete ? core::Result<void>{} :
                                                                        core::Result<void>{Error{ErrorCode::timeout}};
    report.retirement = progress.retirements.empty() ? core::Result<void>{} :
                                                     core::Result<void>{Error{ErrorCode::timeout}};
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until) || Clock::now() >= until) {
        if (progress.ap != RevocationLeg::complete) report.ap = Error{ErrorCode::timeout};
        return report;
    }
    // ControlService derives retirements from the snapshot's identity. Their
    // invalidate_locked calls below clear only that phone's local authority.
    const bool bond_pending = peer && progress.bond != RevocationLeg::complete;
    if (!progress.retirements.empty()) {
        const auto now = Clock::now();
        const int slices = 1 + (progress.ap != RevocationLeg::complete ? 1 : 0) + (bond_pending ? 1 : 0);
        // All closed IDs share one cleanup slice; each unfinished external leg
        // retains its own share even if the first Invalidate reply stalls.
        const auto deadline = std::min(until, now + std::min((until - now) / slices,
            std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(500))));
        report.retirement = {};
        for (const auto& source : progress.retirements) {
            const auto result = impl_->invalidate_locked({source.generation.value, source.request.value}, deadline);
            if (!result) { report.retirement = result.error(); break; }
            ++report.retired;
        }
    }
    if (progress.ap != RevocationLeg::complete) {
        const auto now = Clock::now();
        const auto allowance = bond_pending ? (until - now) / 2 : until - now;
        const auto deadline = std::min(until, now + std::min(allowance,
            std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(500))));
        MessageWriter writer;
        writer.put_u64(snapshot.phone.value);
        const auto result = impl_->rpc("RevokeAP1", "t", writer, deadline);
        report.ap = result ? core::Result<void>{} : core::Result<void>{result.error()};
    }
    if (bond_pending) {
        const auto deadline = std::min(until, Clock::now() + std::chrono::milliseconds(500));
        report.bond = impl_->revoke(*peer, deadline);
    }
    return report;
}
core::Result<State> BusClient::status(Action action) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    if (action != Action::ap_status && action != Action::usb_status && action != Action::modem_status)
        return Error{ErrorCode::invalid_argument};
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until) || Clock::now() >= until) return Error{ErrorCode::timeout};
    return impl_->execute({action, 0, 0}, until);
}
core::Result<State> BusClient::reconnect(const trust::TransportIdentity& identity) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    const auto key = trust::validated_key(identity);
    if (!key || impl_->store.lookup(trust::PhoneIdentity{key.value()}) != trust::Decision::approved)
        return Error{ErrorCode::trust_unknown_phone};
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until) || Clock::now() >= until) return Error{ErrorCode::timeout};
    const auto peer = impl_->observe(until);
    if (!peer) return peer.error();
    const trust::TransportIdentity observed{trust::WirelessIdentity{peer.value().identity.address, peer.value().identity.adapter}};
    if (trust::identity_key(observed) != key.value()) return Error{ErrorCode::trust_unknown_phone};
    MessageWriter writer; writer.put_u64(peer.value().handle);
    const auto result = impl_->rpc("ReconnectPeerV1", "t", writer, until);
    if (!result) return result.error();
    if (result.value() != static_cast<std::uint64_t>(State::active)) return Error{ErrorCode::helper_rejected};
    return State::active;
}
core::Result<std::optional<PeerIdentity>> BusClient::prepare(core::ConsumerId generation, core::PairingRequestId source) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    const auto peer = impl_->observe(until);
    if (!peer) return peer.error();
    MessageWriter writer;
    writer.put_u64(generation.value).put_u64(source.value).put_u64(peer.value().handle);
    const auto bound = impl_->rpc("BindPeerV1", "ttt", writer, until);
    if (!bound) return bound.error();
    return std::optional<PeerIdentity>{peer.value().identity};
}
core::Result<Response> BusClient::call(const Request& request, core::Milliseconds timeout,
                                      core::CancellationToken cancellation) {
    const auto began = Clock::now();
    if (cancellation.stop_requested()) return Error{ErrorCode::cancelled};
    if (timeout.count <= 0) return Error{ErrorCode::timeout};
    const auto until = began + std::chrono::milliseconds(std::min<std::int64_t>(timeout.count, 1000));
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    while (!lock.owns_lock()) {
        if (cancellation.stop_requested()) return Error{ErrorCode::cancelled};
        if (Clock::now() >= until) return Error{ErrorCode::timeout};
        (void)lock.try_lock_until(std::min(until, Clock::now() + std::chrono::milliseconds(5)));
    }
    if (cancellation.stop_requested()) return Error{ErrorCode::cancelled};
    if (Clock::now() >= until) return Error{ErrorCode::timeout};
    // Declared after lock: callback destruction waits for any running interrupt
    // before ownership can pass to another transaction.
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
    bool poll_ap = false;
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
        if (found == impl_->pending.end()) {
            if (action != Action::acquire_ap || impl_->ap_source != source) return Error{ErrorCode::helper_rejected};
            poll_ap = true;
        }
        if (cancellation.stop_requested()) return Error{ErrorCode::cancelled};
        if (Clock::now() >= until) return Error{ErrorCode::timeout};
        if (!poll_ap) {
            grant = found->grant;
            impl_->pending.erase(found);
        }
    }
    if (action == Action::acquire_ap && request.channel != 36) return Error{ErrorCode::helper_rejected};
    const auto result = impl_->execute({poll_ap ? Action::ap_status : action,
        action == Action::acquire_ap && !poll_ap ? request.channel : 0U, grant}, until);
    if (cancellation.stop_requested()) return Error{ErrorCode::cancelled};
    if (!result) return result.error();
    if (action == Action::acquire_ap) {
        if (!poll_ap && result.value() != State::unavailable) impl_->ap_source = impl_->active_source;
        return Response{result.value() == State::active};
    }
    return Response{result.value() != State::unavailable};
}
}
