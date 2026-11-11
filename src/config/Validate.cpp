// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/config/Config.hpp>

#include <cctype>

namespace aa::config {

core::Result<Config> validate(Config candidate) {
    if (candidate.credential_path.empty()) {
        return Error{ErrorCode::config_invalid};
    }
    if (candidate.wireless.jurisdiction.size() != 2 ||
        !std::isupper(static_cast<unsigned char>(candidate.wireless.jurisdiction[0])) ||
        !std::isupper(static_cast<unsigned char>(candidate.wireless.jurisdiction[1]))) {
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
