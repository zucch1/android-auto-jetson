// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>

namespace aa::helper {
// Called only by the receiver's control authorization boundary, after trust
// approval succeeds. This is not an externally dispatched session method.
class Confirmation {
public:
    virtual ~Confirmation() = default;
    virtual core::Result<void> confirmed(core::ConsumerId generation,
                                         core::PairingRequestId source) = 0;
    virtual void cancel(core::ConsumerId generation, core::PairingRequestId source) = 0;
    virtual void teardown(core::ConsumerId generation) = 0;
    virtual void forgotten() = 0;
};
} // namespace aa::helper
