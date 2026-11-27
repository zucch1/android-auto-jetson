// SPDX-License-Identifier: GPL-3.0-or-later
// Real hardware implementation of the AOA link factory (task 28): device
// discovery by port path, the AOA switch over libusb control transfers,
// accessory re-enumeration wait and the bulk-pipe claim. All libusb usage stays
// behind the dlopen ABI (LibusbAbi.hpp). Private to the adapter.
#pragma once

#include <aa/core/Deadline.hpp>
#include <aa/core/Result.hpp>

#include "UsbAoaDevice.hpp"

#include <functional>
#include <memory>

namespace aa::transport::usb_adapter {

// Optional injectable monotonic clock and sleep seam for deterministic tests
// of bounded USB re-enumeration windows (default: steady_clock + cv wait).
struct LibusbClock final {
    std::function<core::Deadline::Clock::time_point()> now{};
    std::function<void(core::Deadline::Clock::time_point until,
                       const core::CancellationToken &stop)>
        wait_until{};
};

// Load the libusb ABI and open a device context; transport_unavailable when no
// runtime libusb is present.
[[nodiscard]] core::Result<std::unique_ptr<AoaLinkFactory>> make_libusb_link_factory();

[[nodiscard]] core::Result<std::unique_ptr<AoaLinkFactory>>
make_libusb_link_factory_with_clock(LibusbClock clock);

} // namespace aa::transport::usb_adapter
