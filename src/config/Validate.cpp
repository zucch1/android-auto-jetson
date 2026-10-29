// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/config/Config.hpp>

namespace aa::config {

core::Result<Config> validate(Config candidate) {
    if (candidate.credential_path.empty()) {
        return Error{ErrorCode::config_invalid};
    }
    if (candidate.wireless.jurisdiction.empty()) {
        return Error{ErrorCode::config_invalid};
    }
    if (!candidate.wireless.jurisdiction_confirmed) {
        return Error{ErrorCode::config_jurisdiction_unconfirmed};
    }
    const auto channel = candidate.wireless.channel;
    if (channel != 36 && channel != 40 && channel != 44 && channel != 48) {
        return Error{ErrorCode::config_invalid};
    }
    return candidate;
}

} // namespace aa::config
