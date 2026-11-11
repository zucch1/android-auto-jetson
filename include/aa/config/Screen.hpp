// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace aa::config {
struct ScreenInsets final {
    std::uint32_t top{}, bottom{}, left{}, right{};
    friend bool operator==(const ScreenInsets&, const ScreenInsets&) = default;
};
// One screen description shared by every offered stream profile. Optional
// fields retain proto2 absence so the legacy default announcement is unchanged.
struct Screen final {
    std::uint32_t density{160};
    std::optional<std::uint32_t> real_density{}, pixel_aspect_ratio_e4{},
        decoder_additional_depth{}, width_margin{}, height_margin{}, theme{};
    std::optional<ScreenInsets> margins{}, content_insets{}, stable_content_insets{};
    friend bool operator==(const Screen&, const Screen&) = default;
};
// Empty means valid; otherwise a stable field name suitable for startup errors.
[[nodiscard]] std::string_view invalid_screen_field(const Screen&, std::uint32_t width,
                                                     std::uint32_t height);
// Existing receiver CLI boundary: --screen-<field> VALUE, strict decimal only.
// Failure leaves the candidate unchanged. Full geometric checks follow parsing.
[[nodiscard]] bool parse_screen_option(Screen&, std::string_view field, std::string_view value);
} // namespace aa::config
