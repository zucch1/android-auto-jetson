// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test-only doubles for the task-27 trust seams: a manual clock so the 60 s
// pairing window is deterministic (no sleep-based tests) and a self-cleaning
// temp directory for store files.

#include <aa/trust/PairingPolicy.hpp>
#include <aa/trust/Store.hpp>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace aa::trust::test {

class ManualClock final : public Clock {
public:
    [[nodiscard]] core::Nanoseconds now() const noexcept override { return now_; }
    void set(core::Nanoseconds t) noexcept { now_ = t; }
    void advance(core::Milliseconds delta) noexcept {
        now_ = core::Nanoseconds{now_.count + core::to_nanoseconds(delta).count};
    }

private:
    core::Nanoseconds now_{};
};

class TempStoreDir final {
public:
    TempStoreDir() {
        static std::uint64_t counter = 0;
        const auto base = std::filesystem::temp_directory_path() /
                          ("aa-trust-" + std::to_string(::getpid()) + "-" +
                           std::to_string(counter++));
        std::filesystem::create_directories(base);
        dir_ = base;
    }
    ~TempStoreDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempStoreDir(const TempStoreDir&) = delete;
    TempStoreDir& operator=(const TempStoreDir&) = delete;
    TempStoreDir(TempStoreDir&&) = delete;
    TempStoreDir& operator=(TempStoreDir&&) = delete;

    [[nodiscard]] std::filesystem::path file() const {
        return dir_ / "android-auto-receiver" / "approved-phones.json";
    }
    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }

private:
    std::filesystem::path dir_{};
};

inline const WiredIdentity kWired{
    "18d1", "4ee1", "SERIAL-42", "Google", "Pixel", "AOA-42", "1-2.3"};

inline const WirelessIdentity kWireless{"AA:BB:CC:DD:EE:FF", "hci0"};

inline PhoneIdentity identity_of(const TransportIdentity& identity) {
    return PhoneIdentity{identity_key(identity)};
}

} // namespace aa::trust::test
