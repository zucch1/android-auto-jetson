// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Identity.hpp>
#include <aa/helper/ObservedPeer.hpp>
#include <aa/helper/ApSnapshot.hpp>
#include <chrono>
#include <optional>
#include <vector>

namespace aa::helper {
enum class Action : std::uint32_t {
    acquire_ap = 1, open_pairing, release_ap, ap_status, usb_status,
    modem_status, register_profile, unregister_profile, reconnect, close_pairing
};
enum class State : std::uint32_t { inactive, active, unavailable };
struct Invocation final {
    Action action{};
    std::uint32_t channel{};
    std::uint64_t grant{};
};
struct Source final {
    std::uint64_t generation{};
    std::uint64_t confirmation{};
    friend bool operator==(const Source&, const Source&) = default;
};
struct SnapshotAuthority final {
    Source source;
    std::uint64_t handle;
    std::uint64_t generation;
};
class Backend {
public:
    virtual ~Backend() = default;
    virtual core::Result<State> apply(Action action, std::uint32_t channel) = 0;
    // Internal cleanup uses the stored lease, not authentication of a dead PID.
    virtual void cleanup() noexcept = 0;
    // Retire active lease effects without erasing other sources' pending observations.
    // Backends without observations can use full cleanup.
    virtual void retire_lease() noexcept { cleanup(); }
    virtual void tick() {}
    virtual core::Result<void> revoke(std::uint64_t) { return Error{ErrorCode::helper_unavailable}; }
    virtual core::Result<ObservedPeer> observe() { return Error{ErrorCode::helper_unavailable}; }
    virtual core::Result<State> authorize(std::uint64_t) { return Error{ErrorCode::helper_unavailable}; }
    virtual core::Result<State> reconnect_peer(std::uint64_t) { return Error{ErrorCode::helper_unavailable}; }
    virtual core::Result<void> revoke_peer(const PeerIdentity&) { return Error{ErrorCode::helper_unavailable}; }
    virtual void discard(std::uint64_t) {}
    virtual core::Result<ObservedPeer> handoff() { return Error{ErrorCode::helper_rejected}; }
    virtual HandoffFailure handoff_failure() const { return HandoffFailure::none; }
    virtual void fail_handoff(std::uint64_t, HandoffFailure) {}
    virtual core::Result<void> authorize_handoff(std::uint64_t, bool, bool) { return Error{ErrorCode::helper_rejected}; }
    virtual void snapshot_provider(const SnapshotProvider&) {}
    virtual void snapshot_fence(const SnapshotFence&) {}
    virtual core::Result<ApSnapshot> ap_snapshot() { return Error{ErrorCode::helper_unavailable}; }
    virtual core::Result<ApSnapshot> ap_snapshot(const SnapshotDeadline&) { return ap_snapshot(); }
    virtual SnapshotDeadline handoff_deadline() const { return {}; }
    virtual bool snapshot_current(std::uint64_t) { return false; }
    virtual bool with_snapshot(std::uint64_t, const std::function<void()>&) { return false; }
};
class UnavailableBackend final : public Backend {
public:
    core::Result<State> apply(Action action, std::uint32_t channel) override;
    void cleanup() noexcept override {}
};
class SystemHelper final {
public:
    using Time = std::chrono::steady_clock::time_point;
    static constexpr std::size_t kMaxPending = 16;
    static constexpr std::size_t kMaxHistory = 4096;
    static constexpr auto kGrantLifetime = std::chrono::seconds(5);
    SystemHelper(IdentityLookup& identities, Backend& backend)
        : identities_(identities), backend_(backend) {
        backend_.snapshot_provider([this]() -> core::Result<ApSnapshot> {
            if (!receiver_ || !handoff_binding_) return Error{ErrorCode::helper_rejected};
            return ap_credentials(receiver_->unique_name, handoff_binding_->source, handoff_binding_->handle);
        });
        backend_.snapshot_fence([this](std::uint64_t epoch, const std::function<void()>& send) {
            if (!receiver_ || !handoff_binding_) return false;
            return transmit_snapshot(receiver_->unique_name, {handoff_binding_->source, handoff_binding_->handle, epoch}, send);
        });
    }
    ~SystemHelper() { backend_.snapshot_provider({}); backend_.snapshot_fence({}); }
    core::Result<std::uint64_t> issue(const std::string& sender, Source source,
                                      Action action, std::uint32_t channel, Time now);
    core::Result<State> execute(const std::string& sender, Invocation invocation, Time now);
    core::Result<void> invalidate(const std::string& sender, Source source);
    core::Result<void> revoke(const std::string& sender, std::uint64_t phone);
    core::Result<ObservedPeer> observe(const std::string& sender);
    core::Result<void> bind(const std::string& sender, Source source, std::uint64_t handle);
    core::Result<State> reconnect_peer(const std::string& sender, std::uint64_t handle);
    core::Result<void> revoke_peer(const std::string& sender, const PeerIdentity& identity);
    core::Result<ObservedPeer> poll_handoff(const std::string&, Source);
    core::Result<ApSnapshot> ap_credentials(const std::string&, Source, std::uint64_t);
    core::Result<void> authorize_handoff(const std::string&, Source, std::uint64_t, bool allow, bool ready);
    HandoffFailure handoff_failure(const std::string& sender) {
        return authenticate(sender) ? backend_.handoff_failure() : HandoffFailure::none;
    }
    bool transmit_snapshot(const std::string&, SnapshotAuthority, const std::function<void()>&);
    SnapshotDeadline snapshot_deadline() const { return backend_.handoff_deadline(); }
    void tick(Time now);
    void platform_tick() {
        backend_.tick();
        if (backend_.handoff_failure() != HandoffFailure::none) { handoff_binding_.reset(); handoff_snapshot_.reset(); }
    }
    void bus_lost() noexcept;
    std::size_t pending() const noexcept { return grants_.size(); }
private:
    struct Grant {
        std::uint64_t id;
        Source source;
        Action action;
        std::uint32_t channel;
        Time expires;
    };
    struct Issued {
        std::uint64_t id;
        std::string receiver;
        Source source;
        Action action;
    };
    core::Result<void> authenticate(const std::string& sender);
    void expire(Time now);
    IdentityLookup& identities_;
    Backend& backend_;
    std::optional<ReceiverIdentity> receiver_;
    std::vector<Grant> grants_;
    std::vector<Issued> history_;
    std::optional<Source> lease_;
    struct Binding { Source source; std::uint64_t handle; };
    std::vector<Binding> bindings_;
    std::optional<Binding> handoff_binding_;
    std::optional<ApSnapshot> handoff_snapshot_;
    bool handoff_owned(const std::string&, Source, std::uint64_t);
    bool handoff_bound(Source, std::uint64_t);
    core::Result<ApSnapshot> validated_snapshot(Source, std::uint64_t);
    void retire_handoff(std::uint64_t, HandoffFailure);
};
} // namespace aa::helper
