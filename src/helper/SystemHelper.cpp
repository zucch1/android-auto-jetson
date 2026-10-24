// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/SystemHelper.hpp>
#include "crypto_adapter/Crypto.hpp"
#include <algorithm>

namespace aa::helper {
namespace {
bool privileged(Action action) {
    return action == Action::acquire_ap || action == Action::open_pairing;
}
}
core::Result<State> UnavailableBackend::apply(Action action, std::uint32_t) {
    switch (action) {
    case Action::ap_status:
    case Action::usb_status:
    case Action::modem_status: return State::unavailable;
    default: return Error{ErrorCode::helper_unavailable};
    }
}
core::Result<void> SystemHelper::authenticate(const std::string& sender) {
    if (sender.empty() || sender.size() > 255 || sender.front() != ':') return Error{ErrorCode::helper_rejected};
    const auto identity = identities_.resolve(sender);
    if (!identity.has_value() || identity.value().unique_name != sender)
        return Error{ErrorCode::helper_rejected};
    if (receiver_ && *receiver_ != identity.value()) return Error{ErrorCode::helper_rejected};
    if (!receiver_) receiver_ = identity.value();
    return {};
}
void SystemHelper::expire(Time now) {
    std::erase_if(grants_, [now](const Grant& g) { return now >= g.expires; });
}
core::Result<std::uint64_t> SystemHelper::issue(const std::string& sender, Source source,
                                               Action action, std::uint32_t channel, Time now) {
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    expire(now);
    if (action < Action::acquire_ap || action > Action::close_pairing)
        return Error{ErrorCode::helper_rejected};
    if (action == Action::reconnect) return Error{ErrorCode::helper_rejected};
    if (!privileged(action) || !source.generation || !source.confirmation ||
        channel != (action == Action::acquire_ap ? 36U : 0U) ||
        grants_.size() >= kMaxPending || history_.size() >= kMaxHistory) {
        return Error{ErrorCode::helper_rejected};
    }
    for (const auto& issued : history_) {
        if (issued.receiver == sender && issued.source == source && issued.action == action)
            return Error{ErrorCode::helper_rejected};
    }
    const auto random = crypto_adapter::random_id();
    if (!random) return random.error();
    const auto id = random.value();
    if (id == source.confirmation) return Error{ErrorCode::helper_rejected};
    // Random collision is still handled fail-closed.
    for (const auto& issued : history_) if (issued.id == id) return Error{ErrorCode::helper_rejected};
    history_.push_back({id, sender, source, action});
    grants_.push_back({id, source, action, channel, now + kGrantLifetime});
    return id;
}
core::Result<State> SystemHelper::execute(const std::string& sender, Invocation invocation, Time now) {
    const auto began = std::chrono::steady_clock::now();
    const auto [action, channel, grant] = invocation;
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    expire(now + (std::chrono::steady_clock::now() - began));
    if (action < Action::acquire_ap || action > Action::close_pairing)
        return Error{ErrorCode::helper_rejected};
    if (channel != (action == Action::acquire_ap ? 36U : 0U))
        return Error{ErrorCode::helper_rejected};
    std::optional<Time> grant_expiry;
    std::optional<Source> grant_source;
    if (privileged(action)) {
        const auto found = std::find_if(grants_.begin(), grants_.end(),
                                       [grant](const Grant& g) { return g.id == grant; });
        if (found == grants_.end() || found->action != action || found->channel != channel)
            return Error{ErrorCode::helper_rejected};
        if (lease_ && lease_->generation != 0 && *lease_ != found->source)
            return Error{ErrorCode::helper_rejected};
        grant_expiry = found->expires;
        grant_source = found->source;
        grants_.erase(found); // consume BEFORE revalidation and backend effects
    } else if (grant != 0) {
        return Error{ErrorCode::helper_rejected};
    }
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    if (grant_expiry && now + (std::chrono::steady_clock::now() - began) >= *grant_expiry)
        return Error{ErrorCode::helper_rejected};
    // Conservatively remember ownership even if a backend reports a partial
    // failure, so loss cleanup never depends on a success reply.
    if (action != Action::ap_status && action != Action::usb_status && action != Action::modem_status)
        lease_ = grant_source.value_or(lease_.value_or(Source{}));
    if (action == Action::open_pairing) {
        const auto binding = std::find_if(bindings_.begin(), bindings_.end(),
            [&grant_source](const Binding& b) { return grant_source && b.source == *grant_source; });
        if (binding == bindings_.end()) return Error{ErrorCode::helper_rejected};
        const auto handle = binding->handle;
        bindings_.erase(binding);
        return backend_.authorize(handle);
    }
    if (action == Action::reconnect) return Error{ErrorCode::helper_rejected};
    return backend_.apply(action, channel);
}
core::Result<void> SystemHelper::invalidate(const std::string& sender, Source source) {
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    if (handoff_binding_ && (!source.generation || source.generation == handoff_binding_->source.generation) &&
        (!source.confirmation || source.confirmation == handoff_binding_->source.confirmation)) {
        (void)backend_.authorize_handoff(handoff_binding_->handle, false, false);
        handoff_binding_.reset();
        handoff_snapshot_.reset();
    }
    std::erase_if(grants_, [source](const Grant& g) {
        return (!source.generation || g.source.generation == source.generation) &&
               (!source.confirmation || g.source.confirmation == source.confirmation);
    });
    std::erase_if(bindings_, [this, source](const Binding& b) {
        const bool matches = (!source.generation || b.source.generation == source.generation) &&
                             (!source.confirmation || b.source.confirmation == source.confirmation);
        if (matches) backend_.discard(b.handle);
        return matches;
    });
    if (lease_ && (!source.generation || !lease_->generation || source.generation == lease_->generation) &&
        (!source.confirmation || source.confirmation == lease_->confirmation)) {
        // Matching bindings were discarded above; a single-source retirement
        // must not destroy observations still bound to other live sources.
        if (source.confirmation) backend_.retire_lease();
        else backend_.cleanup(); // wildcard teardown retains full cleanup
        lease_.reset();
    }
    return {};
}
void SystemHelper::tick(Time now) {
    expire(now);
    if (!receiver_) return;
    const auto current = identities_.resolve(receiver_->unique_name);
    if (!current || current.value() != *receiver_) bus_lost();
}
core::Result<void> SystemHelper::revoke(const std::string& sender, std::uint64_t phone) {
    if (!authenticate(sender) || !phone || phone > 4096) return Error{ErrorCode::helper_rejected};
    // Phone IDs cannot identify Source-keyed authority here. The receiver retires
    // matching sources through Invalidate; AP rotation must preserve unrelated
    // grants, bindings and their BlueZ cleanup ownership.
    return backend_.revoke(phone);
}
void SystemHelper::bus_lost() noexcept {
    backend_.cleanup();
    lease_.reset();
    grants_.clear();
    bindings_.clear();
    handoff_binding_.reset();
    handoff_snapshot_.reset();
    receiver_.reset();
    // Never evict replay history. Exhaustion requires an explicit restart.
}
core::Result<ObservedPeer> SystemHelper::observe(const std::string& sender) {
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    return backend_.observe();
}
core::Result<void> SystemHelper::bind(const std::string& sender, Source source, std::uint64_t handle) {
    if (!authenticate(sender) || !source.generation || !source.confirmation || !handle ||
        bindings_.size() >= kMaxPending) return Error{ErrorCode::helper_rejected};
    for (const auto& b : bindings_) if (b.source == source) return Error{ErrorCode::helper_rejected};
    bindings_.push_back({source, handle});
    return {};
}
core::Result<State> SystemHelper::reconnect_peer(const std::string& sender, std::uint64_t handle) {
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    const auto result = backend_.reconnect_peer(handle);
    if (!result) return result.error();
    handoff_binding_.reset();
    handoff_snapshot_.reset();
    lease_ = Source{};
    return result;
}
core::Result<void> SystemHelper::revoke_peer(const std::string& sender, const PeerIdentity& identity) {
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    return backend_.revoke_peer(identity);
}
core::Result<ObservedPeer> SystemHelper::poll_handoff(const std::string& sender, Source source) {
    if (!authenticate(sender) || !source.generation || !lease_) return Error{ErrorCode::helper_rejected};
    if (*lease_ != Source{} && *lease_ != source) return Error{ErrorCode::helper_rejected};
    if (*lease_ == Source{} && source.confirmation) return Error{ErrorCode::helper_rejected};
    auto peer = backend_.handoff();
    if (!peer) return peer.error();
    if (handoff_binding_ && handoff_binding_->handle != peer.value().handle) {
        handoff_binding_.reset(); handoff_snapshot_.reset();
    }
    if (handoff_binding_ && (handoff_binding_->source != source || handoff_binding_->handle != peer.value().handle))
        return Error{ErrorCode::helper_rejected};
    handoff_binding_ = Binding{source, peer.value().handle};
    if (*lease_ == Source{}) lease_ = source;
    if (handoff_snapshot_ && !validated_snapshot(source, peer.value().handle)) return Error{ErrorCode::helper_rejected};
    return peer;
}
void SystemHelper::retire_handoff(std::uint64_t handle, HandoffFailure reason) {
    backend_.fail_handoff(handle, reason);
    if (handoff_binding_ && handoff_binding_->handle == handle) {
        handoff_binding_.reset(); handoff_snapshot_.reset();
    }
}
// One full identity resolution per request entry and another immediately before
// a secret leaves the helper; intermediate checks reuse no digest across them.
bool SystemHelper::handoff_owned(const std::string& sender, Source source, std::uint64_t handle) {
    return authenticate(sender) && handoff_bound(source, handle);
}
bool SystemHelper::handoff_bound(Source source, std::uint64_t handle) {
    if (!handoff_binding_ || handoff_binding_->source != source || handoff_binding_->handle != handle || !handle)
        return false;
    const auto peer = backend_.handoff();
    return peer && peer.value().handle == handle;
}
core::Result<ApSnapshot> SystemHelper::ap_credentials(const std::string& sender, Source source, std::uint64_t handle) {
    if (!authenticate(sender)) return Error{ErrorCode::helper_rejected};
    return validated_snapshot(source, handle);
}
core::Result<ApSnapshot> SystemHelper::validated_snapshot(Source source, std::uint64_t handle) {
    if (!handoff_bound(source, handle)) return Error{ErrorCode::helper_rejected};
    const auto deadline = backend_.handoff_deadline();
    if (deadline.expired()) { retire_handoff(handle, HandoffFailure::timeout); return Error{ErrorCode::helper_rejected}; }
    auto snapshot = backend_.ap_snapshot(deadline);
    if (deadline.expired()) {
        retire_handoff(handle, HandoffFailure::timeout); return Error{ErrorCode::helper_rejected};
    }
    if (!snapshot) {
        retire_handoff(handle, HandoffFailure::snapshot); return Error{ErrorCode::helper_rejected};
    }
    if (!handoff_bound(source, handle) || !backend_.snapshot_current(snapshot.value().generation)) {
        retire_handoff(handle, HandoffFailure::snapshot);
        return Error{ErrorCode::helper_rejected};
    }
    if (handoff_snapshot_ && *handoff_snapshot_ != snapshot.value()) {
        retire_handoff(handle, HandoffFailure::snapshot); return Error{ErrorCode::helper_rejected};
    }
    handoff_snapshot_ = snapshot.value();
    return snapshot;
}
core::Result<void> SystemHelper::authorize_handoff(const std::string& sender, Source source, std::uint64_t handle,
                                                  bool allow, bool ready) {
    if (!handoff_owned(sender, source, handle)) return Error{ErrorCode::helper_rejected};
    const auto result = backend_.authorize_handoff(handle, allow, ready);
    if ((!allow || !result) && handoff_binding_ && handoff_binding_->handle == handle) handoff_snapshot_.reset();
    return result;
}
bool SystemHelper::transmit_snapshot(const std::string& sender, SnapshotAuthority authority,
                                      const std::function<void()>& send) {
    if (!handoff_bound(authority.source, authority.handle)) return false;
    const auto deadline = backend_.handoff_deadline();
    bool transmitted{};
    if (backend_.with_snapshot(authority.generation, [&] {
        if (!deadline.expired() && handoff_owned(sender, authority.source, authority.handle)) { send(); transmitted = true; }
    }) && transmitted) return true;
    retire_handoff(authority.handle, HandoffFailure::snapshot); return false;
}
} // namespace aa::helper
