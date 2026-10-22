// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Identity.hpp>
#include <aa/helper/ObservedPeer.hpp>
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
        : identities_(identities), backend_(backend) {}
    core::Result<std::uint64_t> issue(const std::string& sender, Source source,
                                      Action action, std::uint32_t channel, Time now);
    core::Result<State> execute(const std::string& sender, Invocation invocation, Time now);
    core::Result<void> invalidate(const std::string& sender, Source source);
    core::Result<void> revoke(const std::string& sender, std::uint64_t phone);
    core::Result<ObservedPeer> observe(const std::string& sender);
    core::Result<void> bind(const std::string& sender, Source source, std::uint64_t handle);
    core::Result<State> reconnect_peer(const std::string& sender, std::uint64_t handle);
    core::Result<void> revoke_peer(const std::string& sender, const PeerIdentity& identity);
    void tick(Time now);
    void platform_tick() { backend_.tick(); }
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
};
} // namespace aa::helper
