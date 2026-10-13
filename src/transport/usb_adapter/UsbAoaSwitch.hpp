// SPDX-License-Identifier: GPL-3.0-or-later
// Android Open Accessory switch (task 28): the bounded handshake algorithm of
// tools/target/usb_aoa_libusb.py handshake(), ported to C++ over an abstract
// control channel so the sequence is unit-testable without hardware. Request
// codes and the version contract mirror the checked-in AASDK reference
// (third_party/aasdk AccessoryMode*Query: GET_PROTOCOL=51, SEND_STRING=52,
// START=53 with vendor-specific IN 0xC0 / OUT 0x40). Private to the adapter.
#pragma once

#include <aa/core/Result.hpp>
#include <aa/transport/UsbAoa.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aa::transport::usb_adapter {

// AOA accessory-mode control requests (Android Open Accessory 2.0).
inline constexpr std::uint8_t kAoaRequestGetProtocol = 51;
inline constexpr std::uint8_t kAoaRequestSendString = 52;
inline constexpr std::uint8_t kAoaRequestStart = 53;
inline constexpr std::uint8_t kAoaRequestIn = 0xC0;
inline constexpr std::uint8_t kAoaRequestOut = 0x40;
inline constexpr std::uint16_t kAoaProtocolLength = 2;
inline constexpr unsigned kAoaControlTimeoutMs = 2000;

// The SEND_STRING wIndex order mandated by the AOA spec (and the probe).
inline constexpr std::uint16_t kAoaStringManufacturer = 0;
inline constexpr std::uint16_t kAoaStringModel = 1;
inline constexpr std::uint16_t kAoaStringDescription = 2;
inline constexpr std::uint16_t kAoaStringVersion = 3;
inline constexpr std::uint16_t kAoaStringUri = 4;
inline constexpr std::uint16_t kAoaStringSerial = 5;

// Control transfers on the pre-switch device; the only operations the switch
// needs. Real: libusb control transfers; tests: scripted fakes.
class AoaControlChannel {
  public:
    virtual ~AoaControlChannel() = default;
    AoaControlChannel() = default;
    AoaControlChannel(const AoaControlChannel &) = delete;
    AoaControlChannel &operator=(const AoaControlChannel &) = delete;

    // Vendor IN (0xC0) control transfer; returns exactly the received bytes.
    [[nodiscard]] virtual core::Result<std::vector<std::byte>> control_in(std::uint8_t request,
                                                                          std::uint16_t value,
                                                                          std::uint16_t index,
                                                                          std::uint16_t length) = 0;

    // Vendor OUT (0x40) control transfer; returns the transferred byte count.
    [[nodiscard]] virtual core::Result<std::size_t>
    control_out(std::uint8_t request, std::uint16_t value, std::uint16_t index,
                std::span<const std::byte> payload) = 0;
};

// Run the AOA switch: GET_PROTOCOL (exactly 2 bytes, version >= 1), SEND_STRING
// x6 (each full length, NUL-terminated ASCII payload), START (0 bytes).
// `report` is the handshake evidence record and is filled on BOTH paths: on
// success started=true and failed_at is empty; on failure `failed_at` names the
// first failed step (probe-compatible step names) and the returned Error is
// typed:
//   control I/O failure            -> transport_io
//   truncated GET_PROTOCOL reply   -> transport_malformed_frame
//   protocol version < 1           -> transport_unavailable
//   short SEND_STRING / START      -> transport_io
[[nodiscard]] core::Result<void>
run_aoa_switch(AoaControlChannel &channel, const AoaAccessoryInfo &info, AoaSwitchReport &report);

// The NUL-terminated ASCII payload one SEND_STRING step carries.
[[nodiscard]] core::Result<std::vector<std::byte>> aoa_string_payload(const std::string &text);

} // namespace aa::transport::usb_adapter
