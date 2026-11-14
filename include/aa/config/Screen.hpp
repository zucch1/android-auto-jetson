// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace aa::config {
struct ScreenInsets final {
    std::uint32_t top{}, bottom{}, left{}, right{};
    friend bool operator==(const ScreenInsets&, const ScreenInsets&) = default;
};
// One screen description for the one panel. Pixel quantities (width/height
// margin, the three inset rectangles) are defined in the pixels of the PRIMARY
// (first offered) stream profile and scaled per axis to every other offered
// profile, so the same fraction of the panel is described whichever profile the
// phone picks. Density, real density, pixel aspect, buffering depth and theme
// are panel properties and are sent unchanged. Optional fields retain proto2
// absence so the legacy default announcement is unchanged.
struct Screen final {
    std::uint32_t density{160};
    std::optional<std::uint32_t> real_density{}, pixel_aspect_ratio_e4{},
        decoder_additional_depth{}, width_margin{}, height_margin{}, theme{};
    std::optional<ScreenInsets> margins{}, content_insets{}, stable_content_insets{};
    friend bool operator==(const Screen&, const Screen&) = default;
};
struct ScreenExtent final {
    std::uint32_t width{}, height{};
    friend bool operator==(const ScreenExtent&, const ScreenExtent&) = default;
};
// Empty means valid; otherwise a stable field name suitable for startup errors.
// Checks pixel quantities exactly as given against one stream extent.
[[nodiscard]] std::string_view invalid_screen_field(const Screen&, std::uint32_t width,
                                                     std::uint32_t height);
// Re-expresses pixel quantities from the `from` stream's pixels in the `to`
// stream's pixels: per axis, round half up (x: width margin, left, right;
// y: height margin, top, bottom). Non-pixel fields are copied unchanged.
[[nodiscard]] Screen scale_screen(const Screen&, ScreenExtent from, ScreenExtent to);
// `profiles[0]` is the reference the screen is defined in. Validates the scaled
// screen against EVERY profile; empty means all valid. Empty profile lists are
// invalid ("profiles").
[[nodiscard]] std::string_view invalid_screen_field(const Screen&, std::span<const ScreenExtent> profiles);
// Fields whose receiver-side behaviour does not exist yet (cropping to the
// margins, matching touch mapping, non-square pixels): nonzero stream margins,
// nonzero ui margins, a pixel aspect other than 10000. Empty means the screen
// uses only implemented behaviour. The CLI refuses these unless explicitly
// opted in.
[[nodiscard]] std::string_view experimental_screen_field(const Screen&);
// Existing receiver CLI boundary: --screen-<field> VALUE, strict decimal only.
// Failure leaves the candidate unchanged. Full geometric checks follow parsing.
[[nodiscard]] bool parse_screen_option(Screen&, std::string_view field, std::string_view value);
} // namespace aa::config
