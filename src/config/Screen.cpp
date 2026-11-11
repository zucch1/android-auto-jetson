// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/config/Screen.hpp>
#include <charconv>

namespace aa::config {
std::string_view invalid_screen_field(const Screen& s, std::uint32_t width, std::uint32_t height) {
    if (s.density != 120 && s.density != 160 && s.density != 240 && s.density != 320
        && s.density != 480 && s.density != 640) { return "density"; }
    if (s.real_density && (*s.real_density < 40 || *s.real_density > 1000)) { return "real-density"; }
    if (s.pixel_aspect_ratio_e4 && (*s.pixel_aspect_ratio_e4 < 5000 || *s.pixel_aspect_ratio_e4 > 20000)) {
        return "pixel-aspect-ratio-e4";
    }
    if (s.decoder_additional_depth && *s.decoder_additional_depth > 8) { return "decoder-additional-depth"; }
    if (s.theme && *s.theme > 2) { return "theme"; }
    if (s.width_margin.value_or(0) >= width) { return "width-margin"; }
    if (s.height_margin.value_or(0) >= height) { return "height-margin"; }
    width -= s.width_margin.value_or(0);
    height -= s.height_margin.value_or(0);
    const auto fits = [width, height](const ScreenInsets& i) {
        return i.left < width && i.right < width && i.left < width - i.right
            && i.top < height && i.bottom < height && i.top < height - i.bottom;
    };
    if (s.margins && !fits(*s.margins)) { return "margins"; }
    if (s.content_insets && !fits(*s.content_insets)) { return "content-insets"; }
    if (s.stable_content_insets && !fits(*s.stable_content_insets)) { return "stable-content-insets"; }
    return {};
}
bool parse_screen_option(Screen& screen, std::string_view field, std::string_view value) {
    std::uint32_t number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || value.empty()) { return false; }
    if (field == "density") { screen.density = number; return true; }
    if (field == "real-density") { screen.real_density = number; return true; }
    if (field == "pixel-aspect-ratio-e4") { screen.pixel_aspect_ratio_e4 = number; return true; }
    if (field == "decoder-additional-depth") { screen.decoder_additional_depth = number; return true; }
    if (field == "width-margin") { screen.width_margin = number; return true; }
    if (field == "height-margin") { screen.height_margin = number; return true; }
    if (field == "theme") { screen.theme = number; return true; }

    const auto set_inset = [&](std::string_view prefix, std::optional<ScreenInsets>& insets) {
        if (!field.starts_with(prefix)) { return false; }
        const auto side = field.substr(prefix.size());
        if (side != "top" && side != "bottom" && side != "left" && side != "right") { return false; }
        auto candidate = insets.value_or(ScreenInsets{});
        if (side == "top") { candidate.top = number; }
        if (side == "bottom") { candidate.bottom = number; }
        if (side == "left") { candidate.left = number; }
        if (side == "right") { candidate.right = number; }
        insets = candidate;
        return true;
    };
    return set_inset("margins-", screen.margins) || set_inset("content-insets-", screen.content_insets)
        || set_inset("stable-content-insets-", screen.stable_content_insets);
}
} // namespace aa::config
