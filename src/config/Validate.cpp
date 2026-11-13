// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/config/Config.hpp>

namespace aa::config {
namespace {

// Explicit ASCII A-Z: std::isupper() consults the C locale and can accept
// bytes above 0x7F (e.g. ISO-8859-1 letters) when a process sets one.
constexpr bool is_ascii_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }

} // namespace

core::Result<Config> validate(Config candidate) {
    if (candidate.credential_path.empty()) {
        return Error{ErrorCode::config_invalid};
    }
    if (candidate.wireless.jurisdiction.size() != 2 ||
        !is_ascii_upper(candidate.wireless.jurisdiction[0]) ||
        !is_ascii_upper(candidate.wireless.jurisdiction[1])) {
        return Error{ErrorCode::config_invalid};
    }
    if (!candidate.wireless.jurisdiction_confirmed) {
        return Error{ErrorCode::config_jurisdiction_unconfirmed};
    }
    if (candidate.wireless.channel != 36) {
        return Error{ErrorCode::config_invalid};
    }
    if (candidate.microphone_enabled || candidate.sensors_enabled) {
        return Error{ErrorCode::config_invalid};
    }
    return candidate;
}

} // namespace aa::config
