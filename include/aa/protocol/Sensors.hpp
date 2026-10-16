// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aa::protocol {

struct SensorStartRequest final {
    std::int32_t type{};
    std::uint64_t min_update_period_ms{};
    [[nodiscard]] friend constexpr bool operator==(const SensorStartRequest&,
                                                   const SensorStartRequest&) noexcept = default;
};

[[nodiscard]] core::Result<SensorStartRequest>
decode_sensor_request(std::span<const std::byte> payload);
[[nodiscard]] core::Result<std::vector<std::byte>> encode_sensor_response(bool accepted);
[[nodiscard]] core::Result<std::vector<std::byte>> encode_night_mode_batch(bool night);

} // namespace aa::protocol
