// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureParse.hpp"

#include "FieldAccess.hpp"

#include <aa/replay/Schema.hpp>

namespace aa::replay::detail {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }

} // namespace

core::Result<ServiceEntry> parse_service(const JsonValue& value) {
    if (!value.is_object()) {
        return malformed();
    }
    const bool has_video = !field(value, "video").is_null();
    const bool has_audio = !field(value, "audio").is_null();
    const bool has_buttons = !field(value, "buttons").is_null();
    const bool has_none = !field(value, "none").is_null();
    const int configs = static_cast<int>(has_video) + static_cast<int>(has_audio)
                        + static_cast<int>(has_buttons) + static_cast<int>(has_none);
    if (configs != 1) {
        return malformed();
    }
    if (has_video) {
        if (const auto keys = expect_keys(value, {"id", "role", "video"}); !keys) {
            return keys.error();
        }
    } else if (has_audio) {
        if (const auto keys = expect_keys(value, {"id", "role", "audio"}); !keys) {
            return keys.error();
        }
    } else if (has_buttons) {
        if (const auto keys = expect_keys(value, {"id", "role", "buttons"}); !keys) {
            return keys.error();
        }
    } else {
        if (const auto keys = expect_keys(value, {"id", "role", "none"}); !keys) {
            return keys.error();
        }
    }
    const auto id = u32_of(field(value, "id"));
    const auto role = role_of(field(value, "role"));
    if (!id || !role || id.value() > 255) {
        return malformed();
    }
    ServiceEntry entry;
    entry.id = protocol::ServiceKey{static_cast<std::int32_t>(id.value())};
    entry.role = role.value();
    if (has_video) {
        const auto* profiles = array_of(field(value, "video"));
        if (profiles == nullptr) {
            return malformed();
        }
        protocol::VideoConfiguration configuration;
        for (const auto& item : *profiles) {
            const auto* triple = array_of(item);
            if (triple == nullptr || triple->size() != 3) {
                return malformed();
            }
            const auto width = u16_of((*triple)[0]);
            const auto height = u16_of((*triple)[1]);
            const auto fps = u8_of((*triple)[2]);
            if (!width || !height || !fps) {
                return malformed();
            }
            configuration.profiles.push_back(
                protocol::VideoProfile{width.value(), height.value(), fps.value()});
        }
        entry.configuration = std::move(configuration);
    } else if (has_audio) {
        const auto* profiles = array_of(field(value, "audio"));
        if (profiles == nullptr) {
            return malformed();
        }
        protocol::AudioConfiguration configuration;
        for (const auto& item : *profiles) {
            const auto* triple = array_of(item);
            if (triple == nullptr || triple->size() != 3) {
                return malformed();
            }
            const auto rate = u32_of((*triple)[0]);
            const auto bits = u8_of((*triple)[1]);
            const auto channels = u8_of((*triple)[2]);
            if (!rate || !bits || !channels) {
                return malformed();
            }
            configuration.profiles.push_back(
                protocol::AudioProfile{rate.value(), bits.value(), channels.value()});
        }
        entry.configuration = std::move(configuration);
    } else if (has_buttons) {
        const auto* keycodes = array_of(field(value, "buttons"));
        if (keycodes == nullptr) {
            return malformed();
        }
        protocol::ButtonConfiguration configuration;
        for (const auto& item : *keycodes) {
            const auto keycode = u16_of(item);
            if (!keycode) {
                return malformed();
            }
            configuration.keycodes.push_back(static_cast<std::int32_t>(keycode.value()));
        }
        entry.configuration = std::move(configuration);
    } else {
        entry.configuration = std::monostate{};
    }
    return entry;
}

} // namespace aa::replay::detail
