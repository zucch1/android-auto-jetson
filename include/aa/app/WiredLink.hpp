// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wired link open seam (task 30): the ONE place that turns a physical USB/AOA
// connection into the framed session transport with trust admission already
// applied. It composes the task-28 connection pipeline
// (transport::connect_usb_aoa) with the task-27 TrustStore admission callback
// (transport::admit_via) and the task-16 framed byte transport
// (transport::UsbByteTransport::over_endpoint); it implements none of them.
//
// TRUST: admission is decided exactly once, inside connect_usb_aoa, over the
// captured wired identity - AOA enumeration success is never trust. A phone the
// store does not approve fails closed (transport_unauthorized_peer) and no
// endpoint is ever released. Nothing here approves a phone.
//
// PRIVACY: the returned PhoneIdentity key encodes the raw wired identity
// material; treat it like a raw identifier and cross it into logs only through
// aa::diagnostics (EventField::identifier) or trust::redacted_label().

#include <aa/core/Result.hpp>
#include <aa/transport/Transport.hpp>
#include <aa/transport/UsbAoa.hpp>
#include <aa/trust/Identity.hpp>
#include <aa/trust/Trust.hpp>

#include <memory>

namespace aa::app {

// An admitted wired link: the framed transport (kind usb_aoa) over the AOA
// accessory endpoint, plus the session-side phone identity derived from the
// captured wired identity (the exact key the TrustStore matches on). The raw
// descriptor fields are deliberately NOT retained here; pass the link to
// session::Lifecycle via WiredSession::phone_discovered(link.phone).
struct WiredLink final {
    std::unique_ptr<transport::Transport> transport{};
    trust::PhoneIdentity phone{};
};

// The session-side identity key for a captured wired identity:
// PhoneIdentity{trust::validated_key(TransportIdentity{identity})}. This is the
// key ApprovedPhoneStore::lookup matches, so the admission decision made inside
// connect_usb_aoa and the session's phone_discovered event name the SAME
// record. Fails invalid_argument when the identity carries no usable material.
[[nodiscard]] core::Result<trust::PhoneIdentity>
wired_phone_identity(const trust::WiredIdentity &identity);

// Discover the device on the selected port, run the AOA switch, admit the
// captured identity through the TrustStore (transport::admit_via), claim the
// accessory bulk pipe and wrap it in the framed USB transport. Any
// connect_usb_aoa failure (including transport_unauthorized_peer for a phone
// the store does not approve) is returned typed and no transport is produced.
// `attempted_identity` (optional) receives the captured identity when admission
// refuses it - the rejected attempt's identity, for log attribution.
[[nodiscard]] core::Result<WiredLink> open_wired_link(const transport::UsbDeviceSelector &selector,
                                                      const transport::AoaAccessoryInfo &info,
                                                      const trust::TrustStore &store,
                                                      trust::WiredIdentity *attempted_identity = nullptr);

} // namespace aa::app
