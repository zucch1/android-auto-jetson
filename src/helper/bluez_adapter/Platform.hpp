// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/BluezBackend.hpp>
#include "ManagedPeers.hpp"
#include <aa/helper/Bootstrap.hpp>
#include <gio/gio.h>
#include <chrono>
#include <map>
#include <optional>
#include <vector>

namespace aa::helper {
inline constexpr auto kAaUuid = "4de17a00-52cb-11e6-bdf4-0800200c9a66";
inline constexpr auto kProfilePath = "/org/custom/AndroidAutoSystemHelper/aa_profile";
inline constexpr auto kHfpUuid = "0000111e-0000-1000-8000-00805f9b34fb";
inline constexpr auto kAgentPath = "/org/custom/AndroidAutoSystemHelper/aa_agent";
struct BluezBackend::Impl {
    using Clock = std::chrono::steady_clock;
    Now now;
    struct Device {
        std::string path, adapter_path;
        PeerIdentity identity;
        bool bonded{};
    };
    struct Observation {
        Device device; std::string owner; Clock::time_point expires;
        std::uint64_t owner_epoch{}, device_epoch{}, adapter_epoch{};
    };
    GMainContext* context{};
    GDBusConnection* bus{};
    GDBusNodeInfo* xml{};
    guint profile_export{}, agent_export{};
    guint owner_subscription{};
    guint removal_subscription{}, properties_subscription{};
    std::uint64_t owner_epoch{};
    std::map<std::string, std::uint64_t> incarnations;
    std::string owner;
    bool profile_requested{true};
    bool registered{}, agent_registered{};
    std::vector<std::string> adapters;
    std::map<std::uint64_t, Observation> observations;
    // Bond states (docs/enrolment-ordering.md). Admitted: on `managed`, durably synced, and not
    // on `pending`. Provisional: acknowledged in this enrolment (`acknowledged`) with its
    // `pending` entry kept. Revoked: on `revoked` only. Cleanup follows `pending` alone.
    bluez_adapter::ManagedPeers managed;
    // Bonds an enrolment window may have created that were never admitted, awaiting removal.
    // Persisted (same file rules as `managed`) so a helper restart cannot lose a removal: this is
    // the durable cleanup obligation of every unaccepted or provisional bond.
    bluez_adapter::ManagedPeers pending;
    // Forgotten bonds: revocation authority only, never admission or acceptance of a new bond.
    bluez_adapter::ManagedPeers revoked;
    // Removed from BlueZ already; only the record's rewrite is still failing. Never removed again.
    std::vector<PeerIdentity> removal_done;
    // Removed by cleanup in this helper run (bounded, oldest first). Their bonds are gone and
    // their journal entries dropped; a receiver's retried revocation of one reports success
    // while no device is present. Not authority: nothing is ever removed because of it.
    std::vector<PeerIdentity> cleaned;
    std::optional<Observation> allowed;
    Clock::time_point window{};
    bool pairing_enabled{};
    struct Comparison {
        PairingComparison value;
        Observation observed;
        Clock::time_point deadline;
        GDBusMethodInvocation* invocation;
    };
    std::optional<Comparison> comparison;
    bool comparison_used{};
    bool comparison_accepted{};
    // The current enrolment's bond was acknowledged (provisional) and awaits the receiver's commit.
    bool acknowledged{};
    // The receiver's trust store is the single authority; these files are its reconciled cache
    // (docs/enrolment-ordering.md). Until the first successful ReconcileBondsV1 of this helper run
    // nothing is admitted, no enrolment opens and no cleanup removes a bond.
    bool reconciled{};
    // The receiver's approved wireless identities as of its last reconciliation (plus commits
    // since). Cleanup never removes a bonded device on this list (#74).
    std::vector<PeerIdentity> approved;
    // Acknowledged (provisional) bonds whose window closed without a commit: the receiver's trust
    // write may already be durable, so only its next reconciliation decides. Never admitted, never
    // removed by cleanup. A subset of `pending` in this helper run; empty after a restart, when the
    // whole journal waits for reconciliation instead.
    std::vector<PeerIdentity> awaiting;
    // Managed bonds whose demotion has started but is not yet durably complete: never admitted.
    std::vector<PeerIdentity> refused;
    // BlueZ removal attempts are budgeted per invocation (docs/enrolment-ordering.md, #118): at
    // most kRemovalAttemptBudget attempts, a further one started only while the invocation has
    // spent less than kRemovalTimeBudget, so a slow or failing BlueZ never holds the helper's
    // single owner thread past the receiver's 500 ms reconciliation or 1 s confirm/reconnect
    // deadlines. A failed attempt backs its identity off (1 s doubling to 30 s, from the
    // attempt's completion); a round-robin cursor keeps one identity from starving the rest. What
    // is not attempted stays durably owed in `pending` for a later tick.
    static constexpr std::size_t kRemovalAttemptBudget = 2;
    static constexpr std::chrono::milliseconds kRemovalTimeBudget{100};
    struct RemovalRetry { PeerIdentity peer; std::uint32_t failures{}; Clock::time_point next{}; };
    std::vector<RemovalRetry> removal_retries;
    std::size_t removal_cursor{};
    struct RemovalBudget {
        Clock::time_point started{Clock::now()};
        std::size_t attempts{};
        bool exhausted{};
    };
    // Which removals an invocation may attempt: `tick` (the periodic tick) every identity whose
    // backoff expired; `first` (reconciliation) only identities with no failed attempt in this
    // helper run, so a reply never waits on retries; `own` only the identity passed in.
    enum class Retire { tick, first, own };
    enum class Removal { gone, failed, deferred };
    // One budgeted attempt: disconnects, then RemoveDevice for each BlueZ object of the identity.
    Removal remove_bond(const std::vector<Device>&, const PeerIdentity&, RemovalBudget&, Retire);
    void retire_unaccepted(Retire = Retire::tick, const PeerIdentity* own = nullptr,
                           const std::vector<Device>* known = nullptr);
    // Moves the current window's acknowledged bond to `awaiting` before the window state is cleared.
    void park();
    // Admission authority for services, reconnect and the handoff: reconciled, durably on
    // `managed`, no cleanup obligation, not awaiting and not being demoted. Retries an unconfirmed
    // directory sync once.
    bool admitted(const PeerIdentity&);
    // Moves a managed bond to `revoked`. With owe_removal the device removal is recorded in
    // `pending` first, before any managed state is dropped (journal, then history, then managed).
    // True only once every write is confirmed durable (rename + directory fsync); until then the
    // identity is refused admission, and the next call or reconciliation completes it.
    bool demote(const PeerIdentity&, bool owe_removal = true);
    // Durably drops revocation history for an identity whose fresh enrolment reached the committed
    // state (accepted number, acknowledged bond, the receiver's durable approval); nothing else
    // clears it. Opening, rejecting, cancelling or timing out an enrolment never does.
    bool clear_revocation(const PeerIdentity&);
    bool quiet_pending{};
    int connection{-1};
    std::shared_ptr<Bootstrap> bootstrap;
    std::optional<Observation> connected;
    std::uint64_t handoff_handle{};
    SnapshotProvider snapshots;
    SnapshotFence fence;
    HandoffFailure last_failure{HandoffFailure::none};
    Clock::time_point next_check{};
    Impl(const std::string&, Now, const std::string&, std::uint32_t);
    ~Impl();
    GVariant* call(const char*, const char*, const char*, GVariant*, const GVariantType* = nullptr);
    // Best effort: tears the link down (HFP and the AA channel included) before the bond is removed,
    // so a removal that fails can never leave an unaccepted device connected.
    void disconnect(const Device&);
    bool current_owner();
    bool ensure();
    bool inventory(std::vector<Device>&);
    bool set(const std::string&, const char*, GVariant*);
    bool quiet();
    void close_connection();
    static int socket_fd(GDBusMethodInvocation*);
    bool register_profiles();
    bool unregister_profiles();
    void close_window();
    void close_admission();
    void reject_comparison();
    void reset();
    // tick: also the budgeted removal retries. Request paths pump without them.
    void pump(bool tick = false);
    bool exact(const Observation&, bool require_bond);
    bool permitted(const char*, bool pairing);
    std::optional<Observation> consume(std::uint64_t);
    void watch_continuity();
    void drain_events();
    void lost_path(const std::string&);
    void retire(std::uint64_t, HandoffFailure);
    static void method(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                       GVariant*, GDBusMethodInvocation*, gpointer);
};
}
