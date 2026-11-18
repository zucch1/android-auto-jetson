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
// The phone's HFP Audio Gateway: the one profile an approved reconnect pages (ConnectProfile).
inline constexpr auto kHfpAgUuid = "0000111f-0000-1000-8000-00805f9b34fb";
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
    // `pending` entry kept. Revoked: on `revoked` only. The Bluetooth adapter is shared (#136):
    // the helper never removes, cancels or disconnects-to-remove a pairing; every bond it does not
    // admit stays in BlueZ, refused for every Android Auto path.
    bluez_adapter::ManagedPeers managed;
    // Enrolments opened and not yet committed or released: never admitted. Persisted (same file
    // rules as `managed`) so reconciliation can admit an approval whose commit never landed; any
    // other entry is released, its bond kept. The file keeps its legacy name (pending-removals):
    // entries an older helper wrote are read the same way and never lead to a removal.
    bluez_adapter::ManagedPeers pending;
    // Forgotten bonds: refused for every Android Auto path, their BlueZ bond kept (#136). Never
    // admission or acceptance of a new bond.
    bluez_adapter::ManagedPeers revoked;
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
    // nothing is admitted, no enrolment opens and no journal entry is released.
    bool reconciled{};
    // Acknowledged (provisional) bonds whose window closed without a commit: the receiver's trust
    // write may already be durable, so only its next reconciliation decides. Never admitted, never
    // released when the window closes. A subset of `pending` in this helper run; empty after a restart, when the
    // whole journal waits for reconciliation instead.
    std::vector<PeerIdentity> awaiting;
    // Managed bonds whose demotion has started but is not yet durably complete: never admitted.
    std::vector<PeerIdentity> refused;
    // Drops the identity's journal entry, if any (best effort: a failed rewrite is retried by the
    // next reconciliation). A bond left in BlueZ is kept, refused, and reported with a redacted address.
    void release(const PeerIdentity&, const std::vector<Device>&);
    // Releases the current window's identity, unless its acknowledged bond is parked in `awaiting`.
    void release_window(const std::vector<Device>& = {});
    // Moves the current window's acknowledged bond to `awaiting` before the window state is cleared.
    void park();
    // Admission authority for services, reconnect and the handoff: reconciled, durably on
    // `managed`, not journalled, not awaiting, not being demoted and not revoked. Retries an unconfirmed
    // directory sync once.
    bool admitted(const PeerIdentity&);
    // Moves a managed bond to `revoked` (history, then out of `managed`). Never owes a removal:
    // on the shared adapter the bond stays in BlueZ, refused (#136). True only once every write is
    // confirmed durable (rename + directory fsync); until then the identity is refused admission,
    // and the next call or reconciliation completes it.
    bool demote(const PeerIdentity&);
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
    // Bounded at 150 ms.
    GVariant* call(const char*, const char*, const char*, GVariant*, const GVariantType* = nullptr);
    bool current_owner();
    bool ensure();
    bool inventory(std::vector<Device>&);
    bool set(const std::string&, const char*, GVariant*);
    bool quiet();
    void close_connection();
    static int socket_fd(GDBusMethodInvocation*);
    bool register_profiles();
    bool unregister_profiles();
    void connect_hands_free(const std::string& device);
    void close_window();
    void close_admission();
    void reject_comparison();
    void reset();
    void pump();
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
