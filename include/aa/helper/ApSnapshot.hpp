// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Identity.hpp>
#include <functional>
#include <chrono>
#include <cstddef>
#include <string_view>

namespace aa::helper {
struct SnapshotDeadline final {
    using Clock = std::chrono::steady_clock;
    Clock::time_point until{Clock::time_point::max()};
    std::function<Clock::time_point()> now{Clock::now};
    bool expired() const { return now() >= until; }
};
void wipe(std::string& value) noexcept;
// A WPA2-Personal passphrase is 8 to 63 ASCII characters (IEEE 802.11i). A
// 64-character all-hex value is read by NetworkManager/hostapd as a raw PMK, and any
// value over 63 characters is refused by phones as a passphrase.
inline constexpr std::size_t kWpa2PassphraseMin = 8;
inline constexpr std::size_t kWpa2PassphraseMax = 63;
struct ApSnapshot final {
    std::string ssid, bssid, ip, password;
    std::uint32_t port{};
    std::uint64_t generation{};
    struct Fields {
        std::string_view ssid, password, bssid, ip;
        std::uint32_t port;
        std::uint64_t generation;
    };
    ApSnapshot() = default;
    explicit ApSnapshot(Fields fields)
        : ssid(fields.ssid), bssid(fields.bssid), ip(fields.ip), password(fields.password), port(fields.port), generation(fields.generation) {}
    ApSnapshot(const ApSnapshot& other)
        : ApSnapshot(Fields{other.ssid, other.password, other.bssid, other.ip, other.port, other.generation}) {}
    ApSnapshot(ApSnapshot&& other) noexcept(false)
        : ApSnapshot(Fields{other.ssid, other.password, other.bssid, other.ip, other.port, other.generation}) { wipe(other.password); }
    ApSnapshot& operator=(const ApSnapshot& other) {
        if (this != &other) {
            ssid = other.ssid; bssid = other.bssid; ip = other.ip; port = other.port; generation = other.generation;
            wipe(password); password = other.password;
        }
        return *this;
    }
    ApSnapshot& operator=(ApSnapshot&& other) noexcept(false) {
        if (this != &other) { *this = static_cast<const ApSnapshot&>(other); wipe(other.password); }
        return *this;
    }
    ~ApSnapshot() { wipe(password); }
    friend bool operator==(const ApSnapshot&, const ApSnapshot&) = default;
};
using SnapshotProvider = std::function<core::Result<ApSnapshot>()>;
using SnapshotFence = std::function<bool(std::uint64_t, const std::function<void()>&)>;
enum class HandoffFailure : std::uint32_t {
    none, denied, eligibility, eof, unknown_id, wrong_order, duplicate,
    oversize, truncated, protobuf, phone_rejected, timeout, write, snapshot, rebind
};
// GetBootstrapWaitV1 (contract 1.5.0, #160): why an open reconnect window has no bootstrap yet.
// Diagnostics only; separate from PollBootstrapV1, whose local_failure keeps its 1.4 range so a
// 1.4 receiver never sees a value it rejects. Wire numbers are append-only.
enum class BootstrapWait : std::uint32_t {
    none = 0,               // no open window for this source, a bootstrap exists, or a link is up
    no_bluetooth_link = 1,  // the window's phone has no Bluetooth link; HFP is infotainment's
};
}
