// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <string>

namespace aa::trust {

// Phone trust boundary: approve-once pairing with known-phone reconnect and
// fail-closed unknown-device behaviour.
//
// Identity limits (documented limitation, plan "Phone identity boundary"):
// wired trust binds physical-port access, USB identifiers, AOA identity and
// TLS session state but is NOT cryptographic proof against a spoofed USB
// identifier; wireless trust binds the BlueZ bond plus stored Bluetooth
// identity. A phone is "unknown" when its transport identity is not in the
// approved store.
//
// Ownership: the store is owned by the session, loaded at startup and written
// only through approve_once/forget; lookups are const and happen on the session
// thread. The identity key is opaque and must be redacted (aa::diagnostics)
// before any logging.
enum class Decision { approved, unknown, rejected };

struct PhoneIdentity final {
    // Opaque transport-bound key (USB ids/serial or Bluetooth identity).
    std::string key;
};

class TrustStore {
public:
    virtual ~TrustStore() = default;
    TrustStore() = default;
    TrustStore(const TrustStore&) = delete;
    TrustStore& operator=(const TrustStore&) = delete;
    TrustStore(TrustStore&&) = delete;
    TrustStore& operator=(TrustStore&&) = delete;

    [[nodiscard]] virtual Decision lookup(const PhoneIdentity& identity) const = 0;
    virtual core::Result<void> approve_once(const PhoneIdentity& identity) = 0;
    virtual core::Result<void> forget(const PhoneIdentity& identity) = 0;
};

// Session admission policy: only an approved phone may hold a session. Unknown
// and rejected fail closed.
[[nodiscard]] constexpr bool allows_session(Decision decision) noexcept {
    return decision == Decision::approved;
}

} // namespace aa::trust
