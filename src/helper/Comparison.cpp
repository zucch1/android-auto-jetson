// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/SystemHelper.hpp>

namespace aa::helper {
core::Result<PairingComparison> SystemHelper::comparison(const std::string& sender, Source source) {
    if (!authenticate(sender) || !source.generation || !source.confirmation || !lease_ || *lease_ != source)
        return Error{ErrorCode::helper_rejected};
    return backend_.comparison();
}
core::Result<void> SystemHelper::respond_comparison(const std::string& sender, Source source, ComparisonResponse response) {
    if (!authenticate(sender) || !source.generation || !source.confirmation || !lease_ || *lease_ != source)
        return Error{ErrorCode::helper_rejected};
    return backend_.respond_comparison(response);
}
}
