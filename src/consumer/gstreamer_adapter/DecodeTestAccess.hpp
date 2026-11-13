// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic clock injection for DecodeHelper pump tests. This header is
// not installed and is only meaningful when AA_DECODE_HELPER_TEST_CLOCK is
// defined, which only the aareceiver-consumer-testclock test library does; the
// production aareceiver-consumer library is built without it and contains no
// clock seam at all.
#pragma once

#ifndef AA_DECODE_HELPER_TEST_CLOCK
#error "DecodeTestAccess.hpp is test-only: define AA_DECODE_HELPER_TEST_CLOCK (aareceiver-consumer-testclock)"
#endif

#include <aa/consumer/Decode.hpp>

#include <utility>

namespace aa::consumer {

class DecodeHelperTestAccess final {
public:
    [[nodiscard]] static core::Result<DecodeHelper> open(DecodeConfig config, DecodeHelper::Now now) {
        return DecodeHelper::open_with_clock(std::move(config), std::move(now));
    }

    // Queue a pipeline ERROR message as a decoder failure arriving "now", so a
    // test can land it at an exact point inside pump() (via the injected
    // clock) instead of racing a real decoder error.
    static void post_pipeline_error(DecodeHelper& helper);
};

} // namespace aa::consumer
