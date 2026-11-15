// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/ObservedPeer.hpp>

namespace aa::helper {
struct PairingComparison final {
    std::uint64_t id{};
    std::uint32_t number{};
    PeerIdentity peer;
};
struct ComparisonResponse final {
    std::uint64_t id{};
    bool accept{};
};
}
