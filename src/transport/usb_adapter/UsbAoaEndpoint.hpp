// SPDX-License-Identifier: GPL-3.0-or-later
// Concrete AOA bulk ByteEndpoint (task 28) and the connection pipeline. The
// class lives private to the adapter: the public seam is
// aa::transport::create_usb_aoa_endpoint / connect_usb_aoa (UsbAoa.hpp), which
// hand out plain ByteEndpoint pointers for UsbByteTransport::over_endpoint().
#pragma once

#include <aa/core/Result.hpp>
#include <aa/transport/UsbAoa.hpp>

#include "UsbAoaDevice.hpp"

#include <memory>

namespace aa::transport::usb_adapter {

// ByteEndpoint over one claimed accessory bulk pipe. open() runs the full
// connection pipeline exactly once (device discovery -> AOA switch -> identity
// capture -> admission -> bulk claim); it is idempotent while open and terminal
// after any failure or close() (fresh discovery = a new endpoint). read/write
// are cancellation-aware (<= kBulkTransferTimeoutMs of cancellation latency)
// and a detach surfaces as transport_closed / transport_io - never a crash and
// never a spin.
class UsbAoaEndpoint final : public ByteEndpoint {
  public:
    // `factory` is owned by the endpoint for its whole lifetime (the real
    // LibusbWorld or a scripted test fixture).
    [[nodiscard]] static core::Result<std::unique_ptr<UsbAoaEndpoint>>
    make(std::unique_ptr<AoaLinkFactory> factory, UsbDeviceSelector selector, AoaAccessoryInfo info,
         WiredAdmission admit);

    ~UsbAoaEndpoint() override;

    [[nodiscard]] core::Result<void> open() override;
    [[nodiscard]] core::Result<std::size_t> read(std::span<std::byte> buffer,
                                                 const core::CancellationToken &token) override;
    [[nodiscard]] core::Result<std::size_t> write(std::span<const std::byte> data,
                                                  const core::CancellationToken &token) override;
    void close() noexcept override;

    // Connection record; valid after a successful open().
    [[nodiscard]] const trust::WiredIdentity &identity() const noexcept { return identity_; }
    [[nodiscard]] const UsbDeviceSnapshot &pre_switch() const noexcept { return pre_switch_; }
    [[nodiscard]] const UsbDeviceSnapshot &post_switch() const noexcept { return post_switch_; }
    [[nodiscard]] const AoaSwitchReport &switch_report() const noexcept { return switch_report_; }

  private:
    UsbAoaEndpoint(std::unique_ptr<AoaLinkFactory> factory, UsbDeviceSelector selector,
                   AoaAccessoryInfo info, WiredAdmission admit) noexcept;

    std::unique_ptr<AoaLinkFactory> factory_;
    UsbDeviceSelector selector_;
    AoaAccessoryInfo info_;
    WiredAdmission admit_;
    std::unique_ptr<UsbBulkPipe> pipe_;
    trust::WiredIdentity identity_{};
    UsbDeviceSnapshot pre_switch_{};
    UsbDeviceSnapshot post_switch_{};
    AoaSwitchReport switch_report_{};
    bool opened_{false};
    bool terminal_{false};
};

// The connection pipeline behind open()/connect_usb_aoa(): discovery + switch
// through the owned `factory`, identity capture, admission, bulk claim.
// Exposed for tests that drive a scripted factory; the returned link carries
// the full connection record (endpoint + identity + snapshots + switch report).
[[nodiscard]] core::Result<UsbAoaLink> connect_aoa(std::unique_ptr<AoaLinkFactory> factory,
                                                   const UsbDeviceSelector &selector,
                                                   const AoaAccessoryInfo &info,
                                                   const WiredAdmission &admit);

} // namespace aa::transport::usb_adapter
