// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wired USB/AOA transport (task 28): accessory detection, the Android Open
// Accessory switch, bulk-endpoint byte I/O and wired identity capture. The
// endpoint is a pure ByteEndpoint: framing stays in UsbByteTransport and no
// libusb type appears here (HARD BOUNDARY, see ByteEndpoint.hpp).
//
// Connection pipeline (open()): discover the device on the selected physical
// port -> AOA switch (GET_PROTOCOL/SEND_STRING x6/START) -> capture the phone's
// pre-switch and accessory-mode descriptor identities -> admission over the
// captured WiredIdentity -> claim the accessory bulk pipe (IN 0x81 / OUT 0x01).
//
// TRUST: AOA enumeration success is NEVER trust. The endpoint is released only
// for an identity `admit` accepts; every other case fails closed with
// transport_unauthorized_peer and no usable endpoint. Wired identity is
// physical-access trust (see Identity.hpp): a spoofed USB identifier that
// matches an approved record is outside the wired trust boundary and is not
// claimed as defeated here.
//
// Error mapping (existing codes only, no renumbering):
//   bad selector / over-budget identity text  -> invalid_argument
//   no usable device on the port / no AOA v1  -> transport_unavailable
//   control/bulk I/O failure, short handshake -> transport_io
//   truncated AOA control response            -> transport_malformed_frame
//   accessory re-enumeration wait expired     -> timeout
//   identity rejected by admission            -> transport_unauthorized_peer
//   endpoint closed / device detached         -> transport_closed
//   cancellation token fired mid-operation    -> cancelled

#include <aa/core/Result.hpp>
#include <aa/transport/ByteEndpoint.hpp>
#include <aa/trust/Identity.hpp>
#include <aa/trust/Trust.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aa::transport {

// AOA accessory strings sent with SEND_STRING (wIndex 0..5) during the switch.
// Receiver-side configuration; bounded printable ASCII, NUL-terminated on the
// wire exactly as tools/target/usb_aoa_libusb.py handshake() sends them.
struct AoaAccessoryInfo final {
    std::string manufacturer{"Android"};
    std::string model{"Android Auto"};
    std::string description{"Android Auto"};
    std::string version{"2.0.1"};
    std::string uri{"https://f1xstudio.com"};
    std::string serial{"HU-AAAAAA001"};
};

// Physical device selection for the qualified USB port. `port_context` is the
// kernel's stable port path ("1-2.1") and is required; bus:address and the
// pre-switch descriptor ids optionally pin one device further. The selector
// only LOCATES a device and is never trust: trust is decided solely by the
// admission callback over the captured identity.
struct UsbDeviceSelector final {
    std::string port_context{};
    int bus{-1};                 // USB bus number, or -1 for any
    int address{-1};             // USB device address, or -1 for any
    std::uint16_t vendor_id{0};  // pre-switch idVendor filter, 0 = any
    std::uint16_t product_id{0}; // pre-switch idProduct filter, 0 = any
};

// A device observation captured from USB descriptors. Device-supplied strings
// are untrusted text: captured fields are bounded (see kMaxIdentityFieldBytes)
// and must be redacted (trust::redacted_label) before any logging.
struct UsbDeviceSnapshot final {
    int bus{};
    int address{};
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::string serial{};
    std::string manufacturer{};
    std::string product{};

    [[nodiscard]] friend bool operator==(const UsbDeviceSnapshot &,
                                         const UsbDeviceSnapshot &) noexcept = default;
};

// One SEND_STRING step of the AOA switch (handshake evidence).
struct AoaStringStep final {
    std::uint16_t index{};
    std::size_t expected_bytes{};
    std::size_t observed_bytes{};
};

// AOA switch evidence: mirrors the per-step record of
// tools/target/usb_aoa_libusb.py handshake(). `failed_at` names the first
// failed step ("get-protocol-length", "get-protocol-version",
// "send-string-<name>", "start-short", or an I/O step name) and is empty on
// success. Step names are code constants, never device text.
struct AoaSwitchReport final {
    std::uint16_t protocol_version{};
    std::vector<AoaStringStep> steps{};
    bool started{false};
    std::string failed_at{};
    int requests{0};
};

// Wired identity admission: return true to admit. An identity that is not
// admitted MUST never receive a usable endpoint (fail closed).
using WiredAdmission = std::function<bool(const trust::WiredIdentity &)>;

// TrustStore-backed admission (task-27 store): admits only identities the store
// already holds as approved. Unknown, rejected and unvalidatable identities are
// rejected; nothing is fabricated and nothing is auto-approved.
[[nodiscard]] WiredAdmission admit_via(const trust::TrustStore &store);

// A live wired AOA connection. `endpoint` is open and ready for
// UsbByteTransport::over_endpoint(); the rest is connection evidence.
struct UsbAoaLink final {
    std::unique_ptr<ByteEndpoint> endpoint{};
    trust::WiredIdentity identity{};
    UsbDeviceSnapshot pre_switch{};
    UsbDeviceSnapshot post_switch{};
    AoaSwitchReport switch_report{};
};

// Discover the device on the selected port, run the AOA switch, capture both
// descriptor identities, gate them on `admit`, and claim the accessory bulk
// pipe. Equivalent to create_usb_aoa_endpoint() + open(), returning the full
// connection record. An identity `admit` refuses fails closed with
// transport_unauthorized_peer even when the AOA switch succeeded.
// `attempted_identity` (optional) receives the captured identity when admission
// refuses it - the rejected attempt's identity, for log attribution.
[[nodiscard]] core::Result<UsbAoaLink> connect_usb_aoa(const UsbDeviceSelector &selector,
                                                       const AoaAccessoryInfo &info,
                                                       const WiredAdmission &admit,
                                                       trust::WiredIdentity *attempted_identity = nullptr);

// ByteEndpoint seam: the returned endpoint performs device discovery, the AOA
// switch, identity capture + admission and the bulk claim inside open() (the
// task-28 ByteEndpoint contract). Hand it to UsbByteTransport::over_endpoint(),
// which calls open() and exposes the framed Transport (kind usb_aoa) that
// aa::session::Lifecycle consumes. open() is idempotent; after a failure or
// close() the endpoint is terminal and a fresh discovery needs a new endpoint.
[[nodiscard]] core::Result<std::unique_ptr<ByteEndpoint>>
create_usb_aoa_endpoint(const UsbDeviceSelector &selector, const AoaAccessoryInfo &info,
                        const WiredAdmission &admit);

// Read-only observation of whatever currently sits on the selected port
// (descriptor ids, bus:address; descriptor strings are not fetched). Returns
// transport_unavailable when no device is present. For probes and reconnect
// recovery only; never a trust decision.
[[nodiscard]] core::Result<UsbDeviceSnapshot> observe_usb_device(const UsbDeviceSelector &selector);

// Targeted software reset of the device on the selected port (never a hub or
// controller reset), used to return a device left in accessory mode by an
// earlier run back to phone mode before a fresh switch. The device
// re-enumerates afterwards: callers must re-discover, never reuse handles.
[[nodiscard]] core::Result<void> reset_port_device(const UsbDeviceSelector &selector);

} // namespace aa::transport
