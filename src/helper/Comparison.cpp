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
// Source-less, like RevokePeerV1: it runs at receiver start, before any pairing source or lease
// exists. Sender, UID and process identity are authenticated exactly as for every other method;
// the live lease's enrolment window is never touched by it (the backend skips that bond).
core::Result<std::uint64_t> SystemHelper::reconcile_bonds(const std::string& sender, const std::vector<PeerIdentity>& approved) {
    if (!authenticate(sender) || approved.size() > kMaxApprovedBonds) return Error{ErrorCode::helper_rejected};
    for (std::size_t i = 0; i < approved.size(); ++i)
        for (std::size_t j = i + 1; j < approved.size(); ++j)
            if (approved[i] == approved[j]) return Error{ErrorCode::helper_rejected};
    return backend_.reconcile_bonds(approved);
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
