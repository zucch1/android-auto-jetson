// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Result.hpp>
#include <cstdint>
#include <string>

namespace aa::helper {
// Data, not caller-supplied BlueZ object paths. Handles are helper observations.
struct PeerIdentity final {
    std::string address;
    std::string adapter;
    friend bool operator==(const PeerIdentity&, const PeerIdentity&) = default;
};
struct ObservedPeer final {
    std::uint64_t handle{};
    PeerIdentity identity;
};
}
