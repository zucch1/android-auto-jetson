// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Typed transport identity for approved phones (task 27). The store keeps the
// STRONGEST available transport identity per phone:
//   - wired    : USB descriptor ids + USB serial + AOA identity + physical-port
//                context (the qualified USB3 port path as the kernel reports it)
//   - wireless : the BlueZ bond identity (bond address + adapter) carried as
//                plain DATA. This module never talks to BlueZ; the transport
//                layer hands the bond identity over.
//
// TRUST MODEL — WIRED IDENTITY IS PHYSICAL-ACCESS TRUST, NOT CRYPTOGRAPHIC
// PROOF. USB descriptor strings and serials are attacker-controlled data: a
// device presenting a spoofed USB identifier matches an approved record and is
// indistinguishable at this layer. An approved wired record therefore
// authorizes "the approved identifiers presented on the qualified port", not
// "this specific handset". This is a known, accepted limitation of the wired
// trust boundary and is deliberately NOT claimed as cryptographic device
// attestation. Wireless trust binds the BlueZ bond (stronger, still
// bond-not-human identity).
//
// Matching: identity_key() is a stable, injective encoding of the full
// structured record, so ANY difference in ANY component is a different
// identity and fails closed (Decision::unknown). The full record is persisted
// for audit and revocation.
//
// Privacy (task-19): every label produced for logs/diagnostics is pseudonymized
// through aa::diagnostics; raw serials, Bluetooth MACs and port contexts never
// leave the user-owned store file.

#include <aa/core/Result.hpp>

#include <string>
#include <variant>

namespace aa::trust {

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

// Stable, injective canonical key of the FULL structured identity:
//   wired    -> "w1:" + length-prefixed descriptor, serial, AOA and port fields
//   wireless -> "b1:" + length-prefixed bond address and adapter
//   opaque   -> the stored key verbatim (a migrated legacy record's key IS its
//               identity; the session seam matches PhoneIdentity.key directly)
// Length-prefixed encoding is unambiguous and the kind prefixes keep the
// structured forms distinct from each other. Use validated_key() upstream: it
// rejects identities that carry no usable identity material.
[[nodiscard]] std::string identity_key(const TransportIdentity& identity);

// Field-level validation (printable ASCII, usable material). Fail closed.
[[nodiscard]] core::Result<std::string> validated_key(const TransportIdentity& identity);

// Pseudonymized label for logs/diagnostics (task-19 redaction boundary): never
// contains the raw serial, Bluetooth address or port context.
[[nodiscard]] std::string redacted_label(const TransportIdentity& identity);

} // namespace aa::trust
