// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureValidate.hpp"

#include <limits>

namespace aa::replay::detail {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }

} // namespace

core::Result<void> validate_session_event(session::Event event) {
    if (session::to_string(event) == std::string_view{"unknown"}) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_direction(Direction dir) {
    if (to_string(dir) == std::string_view{"unknown"}) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_audio_role(audio::Role role) {
    if (audio_role_name(role) == std::string_view{"unknown"}) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_channel_role(protocol::ChannelRole role) {
    if (role_name(role) == std::string_view{"unknown"}) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_wire_id(std::uint64_t id) {
    if (id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_provenance(Provenance provenance) {
    if (provenance != Provenance::synthetic) {
        return malformed();
    }
    return {};
}

} // namespace aa::replay::detail
