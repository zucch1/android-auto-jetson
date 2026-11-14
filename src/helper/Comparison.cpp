// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/SystemHelper.hpp>

namespace aa::helper {
core::Result<bool> SystemHelper::bond_confirmed(const std::string& sender, Source source) {
    if (!authenticate(sender) || !source.generation || !source.confirmation || !lease_ || *lease_ != source)
        return Error{ErrorCode::helper_rejected};
    return backend_.bond_confirmed();
}
core::Result<void> SystemHelper::acknowledge_bond(const std::string& sender, Source source) {
    if (!authenticate(sender) || !source.generation || !source.confirmation || !lease_ || *lease_ != source)
        return Error{ErrorCode::helper_rejected};
    return backend_.acknowledge_bond();
}
core::Result<void> SystemHelper::commit_bond(const std::string& sender, Source source) {
    if (!authenticate(sender) || !source.generation || !source.confirmation || !lease_ || *lease_ != source)
        return Error{ErrorCode::helper_rejected};
    return backend_.commit_bond();
}
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
