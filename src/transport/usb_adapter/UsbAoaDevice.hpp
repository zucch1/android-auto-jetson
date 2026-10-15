// SPDX-License-Identifier: GPL-3.0-or-later
// Device-level seams of the wired AOA connection (task 28): the bulk pipe and
// the link factory the connection pipeline drives. The real factory
// (LibusbWorld.cpp) speaks to hardware through the dlopen libusb ABI; unit
// tests provide scripted device fixtures. Private to the adapter.
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/transport/UsbAoa.hpp>
#include <aa/trust/Identity.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace aa::transport::usb_adapter {

// Accessory-mode bulk endpoints (task-11 probe evidence: 512-byte packets).
inline constexpr std::uint8_t kAccessoryBulkIn = 0x81;
inline constexpr std::uint8_t kAccessoryBulkOut = 0x01;
inline constexpr int kAccessoryInterface = 0;

// Bounded bulk wait per transfer; the endpoint checks cancellation between
// attempts, so a stalled peer can never block past one timeout + token fire.
inline constexpr unsigned kBulkTransferTimeoutMs = 200;

// Bounded wait for accessory-mode re-enumeration after START
// (tools/target/usb_aoa_libusb.py REENUMERATE_TIMEOUT_S is 15 s).
inline constexpr core::Milliseconds kAccessoryWaitMs{15'000};

// One claimed accessory bulk pipe (IN 0x81 / OUT 0x01). A zero read/write count
// means the bounded transfer timed out with no data - NOT end-of-stream. A
// typed error means the pipe failed (hotplug returns transport_closed) and the
// pipe is terminal.
class UsbBulkPipe {
  public:
    virtual ~UsbBulkPipe() = default;
    UsbBulkPipe() = default;
    UsbBulkPipe(const UsbBulkPipe &) = delete;
    UsbBulkPipe &operator=(const UsbBulkPipe &) = delete;

    [[nodiscard]] virtual core::Result<std::size_t> read(std::span<std::byte> buffer,
                                                         unsigned timeout_ms) = 0;
    [[nodiscard]] virtual core::Result<std::size_t> write(std::span<const std::byte> data,
                                                          unsigned timeout_ms) = 0;
    virtual void close() noexcept = 0;
};

// Hardware steps behind the connection pipeline. `switch_to_accessory` finds
// the phone on the selected port, runs the AOA switch over it and captures the
// pre-switch descriptor snapshot. `claim_accessory` waits (bounded) for the
// accessory device on the SAME port and claims its bulk pipe, capturing the
// post-switch snapshot. Both fail typed and terminal; handles are never reused
// across a re-enumeration.
class AoaLinkFactory {
  public:
    virtual ~AoaLinkFactory() = default;
    AoaLinkFactory() = default;
    AoaLinkFactory(const AoaLinkFactory &) = delete;
    AoaLinkFactory &operator=(const AoaLinkFactory &) = delete;

    virtual void set_connect_deadline(const core::Deadline &deadline) { deadline_ = deadline; }

    [[nodiscard]] virtual core::Result<void>
    switch_to_accessory(const UsbDeviceSelector &selector, const AoaAccessoryInfo &info,
                        UsbDeviceSnapshot &pre_switch, AoaSwitchReport &switch_report) = 0;

    [[nodiscard]] virtual core::Result<std::unique_ptr<UsbBulkPipe>>
    claim_accessory(const UsbDeviceSelector &selector, UsbDeviceSnapshot &post_switch) = 0;

  protected:
    core::Deadline deadline_{};
};

// Port-path syntax gate for every hardware-facing entry point: the kernel's
// stable form only ("1-2.1"); anything else (traversal, empty, host paths) is
// invalid_argument before any device lookup.
[[nodiscard]] core::Result<void> validate_selector(const UsbDeviceSelector &selector);

// Build the wired identity from both descriptor observations and the port.
//   usb_*   = the phone's pre-switch descriptor (native identity)
//   aoa_*   = the phone's accessory-mode descriptor strings (AOA identity)
//   port    = the qualified physical port path
// Every field is device-supplied text and stays raw here; validated_key() /
// redacted_label() gate it before storage or logging.
[[nodiscard]] trust::WiredIdentity build_wired_identity(const UsbDeviceSnapshot &pre_switch,
                                                        const UsbDeviceSnapshot &post_switch,
                                                        const std::string &port_context);

// Fail-closed identity gate: valid identity material AND `admit` accepts it.
// Anything else (over-budget/unprintable text, unknown, rejected) is refused.
[[nodiscard]] core::Result<void> admit_wired_identity(const trust::WiredIdentity &identity,
                                                      const WiredAdmission &admit);

} // namespace aa::transport::usb_adapter
