// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Typed transport identity for approved phones (task 27). The store keeps the
// STRONGEST available transport identity per phone:
//   - wired    : USB descriptor ids + USB serial + AOA identity, plus the
//                physical-port context (the USB port path as the kernel
//                reports it) the phone was captured on. A serial-backed wired
//                identity MATCHES ON THE PHONE, NOT THE PORT (#133, see
//                match_key()): the same phone is admitted on any host port.
//                The port is still recorded for audit.
//     Serial-less wired identities have a port-only strength: the descriptor
//     tuple on that port matches every indistinguishable device, so it keeps
//     matching on that port only and is never widened to other ports. The
//     store requires a separate explicit approval API for this weaker grant.
//   - wireless : the BlueZ bond identity (bond address + adapter) carried as
//                plain DATA. This module never talks to BlueZ; the transport
//                layer hands the bond identity over.
//
// TRUST MODEL — WIRED IDENTITY IS PHYSICAL-ACCESS TRUST, NOT CRYPTOGRAPHIC
// PROOF. USB descriptor strings and serials are attacker-controlled data: a
// device presenting a spoofed USB identifier matches an approved record and is
// indistinguishable at this layer. An approved serial-backed wired record
// therefore authorizes "the approved identifiers presented on any receiver
// USB port", not "this specific handset". Binding it to one port added no
// security (a spoofing device can use the approved port as easily as any
// other). This is a known, accepted limitation of the wired trust boundary and
// is deliberately NOT claimed as cryptographic device attestation. Wireless
// trust binds the BlueZ bond (stronger, still bond-not-human identity).
//
// Matching: identity_key() is a stable, injective encoding of the full
// structured record (Bluetooth MAC addresses are case-insensitive). The store
// matches on match_key(identity_key()), which only drops the port of a
// serial-bound wired key; any other difference in a component is a different
// identity and fails closed (Decision::unknown). The full record is persisted
// for audit and revocation.
//
// Privacy (task-19): every label produced for logs/diagnostics is pseudonymized
// through aa::diagnostics; raw serials, Bluetooth MACs and port contexts never
// leave the user-owned store file.

#include <aa/core/Result.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace aa::trust {

// Port-only identifies a descriptor tuple on a port, never a particular phone.
enum class IdentityStrength { serial_backed, port_only, bond_backed, opaque };

enum class TransportKind { wired, wireless, opaque };

// Wired transport identity: USB descriptor/serial, AOA identity, port context.
struct WiredIdentity final {
    std::string usb_vendor_id{};
    std::string usb_product_id{};
    std::string usb_serial{};
    std::string aoa_manufacturer{};
    std::string aoa_model{};
    std::string aoa_serial{};
    std::string port_context{};  // physical port path (e.g. "1-2.3")

    [[nodiscard]] friend bool operator==(const WiredIdentity&, const WiredIdentity&) noexcept =
        default;
};

// Wireless transport identity: the BlueZ bond identity as data (no BlueZ API).
struct WirelessIdentity final {
    std::string bond_address{};
    std::string adapter{};

    [[nodiscard]] friend bool operator==(const WirelessIdentity&, const WirelessIdentity&) noexcept =
        default;
};

// Key-only identity carried by migrated legacy records: the structured fields
// were never captured, so the opaque key is all there is to match on.
struct OpaqueIdentity final {
    std::string key{};

    [[nodiscard]] friend bool operator==(const OpaqueIdentity&, const OpaqueIdentity&) noexcept =
        default;
};

using TransportIdentity = std::variant<WiredIdentity, WirelessIdentity, OpaqueIdentity>;

// Per-field serialization budget (gate-fix round 2): matches the store
// reader's string limit (JsonLimits::max_string), so every accepted identity
// round-trips through the on-disk schema. validated_key() rejects longer
// fields BEFORE any mutation commits; over-budget input can never persist a
// document the loader would refuse.
inline constexpr std::size_t kMaxIdentityFieldBytes = 512;

[[nodiscard]] TransportKind kind_of(const TransportIdentity& identity) noexcept;

[[nodiscard]] IdentityStrength strength_of(const TransportIdentity& identity) noexcept;
[[nodiscard]] std::string_view strength_name(IdentityStrength strength) noexcept;

// Stable, injective canonical key of the FULL structured identity:
//   wired    -> "w1:" + length-prefixed descriptor, serial, AOA and port fields
//   wireless -> "b1:" + length-prefixed bond address and adapter
//   opaque   -> the stored key verbatim (a migrated legacy record's key IS its
//               identity; the session seam matches PhoneIdentity.key directly)
// Length-prefixed encoding is unambiguous and the kind prefixes keep the
// structured forms distinct from each other. Use validated_key() upstream: it
// rejects identities that carry no usable identity material.
[[nodiscard]] std::string identity_key(const TransportIdentity& identity);

// Classification of a canonical key against the serial-less wired policy
// (audit M12). It inspects the KEY, never the stored variant, so a "w1:" key
// held as an OpaqueIdentity (approve_once(PhoneIdentity), legacy migration) is
// judged exactly like the structured WiredIdentity that produced it.
//   not_wired    : not a "w1:" key (wireless, or an opaque legacy key)
//   serial_bound : a canonical "w1:" key with a USB or AOA serial
//   port_only    : a canonical "w1:" key with BOTH serials blank
//   malformed    : starts with "w1:" but is not exactly the seven canonical
//                  length-prefixed fields identity_key() emits
enum class WiredKeyClass { not_wired, serial_bound, port_only, malformed };

[[nodiscard]] WiredKeyClass classify_wired_key(std::string_view key) noexcept;

// The pre-switch USB serial (iSerialNumber; on Android the adb serial) of a
// canonical "w1:" key, or nullopt for any other key. The qualification
// receiver uses it to pin a run to one physical phone (#137); the value is a
// raw identifier, so never log it.
[[nodiscard]] std::optional<std::string> wired_key_usb_serial(std::string_view key);

// Trust-matching key (#133). A serial-bound wired key matches on the PHONE
// (descriptor ids + USB serial + AOA identity), not on the port: the port
// field is replaced by the empty field, so the same serial-backed phone is the
// same approval on every host port. Every other key is returned verbatim, so a
// port-only (serial-less) key still matches ONLY on its own port, and a
// malformed "w1:" key never collapses onto another record. The store matches,
// re-approves and revokes by this key; identity_key() stays the persisted,
// audited full record (the port the phone was approved on is kept).
[[nodiscard]] std::string match_key(std::string_view key);
[[nodiscard]] std::string match_key(const TransportIdentity& identity);

// Field-level validation (printable ASCII, usable material). Fail closed.
[[nodiscard]] core::Result<std::string> validated_key(const TransportIdentity& identity);

// Pseudonymized label for logs/diagnostics (task-19 redaction boundary): never
// contains the raw serial, Bluetooth address or port context.
[[nodiscard]] std::string redacted_label(const TransportIdentity& identity);

} // namespace aa::trust
