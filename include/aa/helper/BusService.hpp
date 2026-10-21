// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <memory>

namespace aa::helper {
inline constexpr auto kBusName = "org.custom.AndroidAutoSystemHelper";
inline constexpr auto kObjectPath = "/org/custom/AndroidAutoSystemHelper";
inline constexpr auto kInterface = "org.custom.AndroidAutoSystemHelper1";
class BusService final {
public:
    static core::Result<BusService> system(IdentityPolicy policy, Backend& backend);
    static core::Result<BusService> private_bus(const std::string& address,
                                               IdentityPolicy policy, Backend& backend);
    ~BusService();
    BusService(BusService&&) noexcept;
    BusService& operator=(BusService&&) noexcept;
    core::Result<void> cycle();
private:
    struct Impl;
    explicit BusService(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
