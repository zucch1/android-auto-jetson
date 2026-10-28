// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/helper/ObservedPeer.hpp>
#include <aa/helper/PairingComparison.hpp>
#include <aa/trust/Identity.hpp>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <vector>

namespace aa::helper {
// Captured on the store owner before deletion; never changed during retries.
struct RevocationV1 final {
    core::PhoneId phone;
    trust::TransportIdentity identity;
};
inline std::optional<PeerIdentity> revocation_peer(const RevocationV1& snapshot) {
    return std::visit([](const auto& identity) -> std::optional<PeerIdentity> {
        using T = std::decay_t<decltype(identity)>;
        if constexpr (std::is_same_v<T, trust::WirelessIdentity>) {
            return PeerIdentity{identity.bond_address, identity.adapter};
        } else {
            static_assert(std::is_same_v<T, trust::WiredIdentity> || std::is_same_v<T, trust::OpaqueIdentity>);
            return std::nullopt;
        }
    }, snapshot.identity);
}
enum class RevocationLeg { pending, complete };
struct RetirementSource final {
    core::ConsumerId generation;
    core::PairingRequestId request;
    friend bool operator==(const RetirementSource&, const RetirementSource&) = default;
};
struct RevocationProgress final {
    RevocationLeg ap{RevocationLeg::pending};
    std::optional<RevocationLeg> bond;
    std::vector<RetirementSource> retirements;
    bool complete() const {
        return retirements.empty() && ap == RevocationLeg::complete && (!bond || *bond == RevocationLeg::complete);
    }
};
struct RevocationAttempt final {
    core::Result<void> ap{Error{ErrorCode::helper_unavailable}};
    std::optional<core::Result<void>> bond;
    std::size_t retired{}; // Acknowledged prefix of the supplied pending retirements.
    core::Result<void> retirement{Error{ErrorCode::helper_unavailable}};
};
// Called only by the receiver's control authorization boundary, after trust
// approval succeeds. This is not an externally dispatched session method.
class Confirmation {
public:
    virtual ~Confirmation() = default;
    virtual core::Result<PairingComparison> comparison(core::ConsumerId, core::PairingRequestId) {
        return Error{ErrorCode::helper_rejected};
    }
    virtual core::Result<void> respond_comparison(core::ConsumerId, core::PairingRequestId, ComparisonResponse) {
        return Error{ErrorCode::helper_rejected};
    }
    virtual core::Result<void> confirmed(core::ConsumerId generation,
                                         core::PairingRequestId source) = 0;
    virtual void cancel(core::ConsumerId generation, core::PairingRequestId source) = 0;
    virtual void teardown(core::ConsumerId generation) = 0;
    // One <=1000ms attempt, including mutex acquisition and associated cleanup.
    // Completed legs report success without another privileged request.
    virtual RevocationAttempt forgotten_v1(const RevocationV1&, const RevocationProgress&) = 0;
    virtual core::Result<std::optional<PeerIdentity>> prepare(core::ConsumerId, core::PairingRequestId) {
        return std::optional<PeerIdentity>{};
    }
};
} // namespace aa::helper
