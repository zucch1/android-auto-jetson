// SPDX-License-Identifier: GPL-3.0-or-later
// Real hardware implementation of the AOA link factory (task 28): device
// discovery by port path, the AOA switch over libusb control transfers,
// accessory re-enumeration wait and the bulk-pipe claim. All libusb usage stays
// behind the dlopen ABI (LibusbAbi.hpp). Private to the adapter.
#pragma once

#include <aa/core/Result.hpp>

#include "UsbAoaDevice.hpp"

#include <memory>

namespace aa::transport::usb_adapter {

// Load the libusb ABI and open a device context; transport_unavailable when no
// runtime libusb is present.
[[nodiscard]] core::Result<std::unique_ptr<AoaLinkFactory>> make_libusb_link_factory();

} // namespace aa::transport::usb_adapter
