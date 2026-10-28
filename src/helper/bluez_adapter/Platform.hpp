// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/BluezBackend.hpp>
#include <aa/helper/Bootstrap.hpp>
#include <aa/helper/HfpSlc.hpp>
#include <gio/gio.h>
#include <chrono>
#include <map>
#include <optional>
#include <vector>

namespace aa::helper {
inline constexpr auto kAaUuid = "4de17a00-52cb-11e6-bdf4-0800200c9a66";
inline constexpr auto kProfilePath = "/org/custom/AndroidAutoSystemHelper/aa_profile";
inline constexpr auto kHfpUuid = "0000111f-0000-1000-8000-00805f9b34fb";
inline constexpr auto kHfpPath = "/org/custom/AndroidAutoSystemHelper/hfp_ag_profile";
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
    guint profile_export{}, hfp_export{}, agent_export{};
    guint owner_subscription{};
    guint removal_subscription{}, properties_subscription{};
    std::uint64_t owner_epoch{};
    std::map<std::string, std::uint64_t> incarnations;
    std::string owner;
    bool profile_requested{true};
    bool registered{}, hfp_registered{}, agent_registered{};
    std::vector<std::string> adapters;
    std::map<std::uint64_t, Observation> observations;
    std::vector<PeerIdentity> managed;
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
    bool quiet_pending{};
    int connection{-1};
    std::shared_ptr<Bootstrap> bootstrap;
    std::optional<Observation> connected;
    std::shared_ptr<HfpSlc> hfp;
    std::optional<Observation> hfp_peer;
    Clock::time_point hfp_deadline{}, hfp_check{};
    std::uint64_t handoff_handle{};
    SnapshotProvider snapshots;
    SnapshotFence fence;
    HandoffFailure last_failure{HandoffFailure::none};
    Clock::time_point next_check{};
    Impl(const std::string&, Now);
    ~Impl();
    GVariant* call(const char*, const char*, const char*, GVariant*, const GVariantType* = nullptr);
    bool current_owner();
    bool ensure();
    bool inventory(std::vector<Device>&);
    bool set(const std::string&, const char*, GVariant*);
    bool quiet();
    void close_connection();
    void close_hfp();
    void tick_hfp();
    void hfp_method(GVariant*, GDBusMethodInvocation*);
    static int socket_fd(GDBusMethodInvocation*);
    bool register_profiles();
    bool unregister_profiles();
    void close_window();
    void close_admission();
    void reject_comparison();
    void reset();
    void pump();
    bool exact(const Observation&, bool require_bond);
    bool permitted(const char*, bool pairing);
    std::optional<Observation> consume(std::uint64_t);
    void remember(const PeerIdentity&);
    void watch_continuity();
    void drain_events();
    void lost_path(const std::string&);
    void retire(std::uint64_t, HandoffFailure);
    static void method(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                       GVariant*, GDBusMethodInvocation*, gpointer);
};
}
