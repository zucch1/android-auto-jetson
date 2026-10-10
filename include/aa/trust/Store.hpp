// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Approved-phone trust store (task 27): a user-owned, persistent allowlist of
// phones that completed approve-once pairing. The store keeps the STRONGEST
// available transport identity per phone (see Identity.hpp) and nothing else —
// no credentials, no keys, no certificates: approvals reference identities,
// never secrets.
//
// TRUST MODEL (documented limitation): a wired identity is PHYSICAL-ACCESS
// TRUST, NOT CRYPTOGRAPHIC PROOF. USB descriptor strings and serials can be
// spoofed by anyone with physical access to the port; an approved wired record
// therefore authorizes "the approved identifiers presented on the qualified
// port", not "this specific handset". Wireless records bind the BlueZ bond
// (stronger, still bond-not-human identity). Unknown phones always fail closed:
// lookup() returns Decision::unknown and allows_session() is false.
//
// Persistence: `$XDG_STATE_HOME/android-auto-receiver/approved-phones.json`
// (XDG fallback `~/.local/state/...`), created 0600 and re-enforced to 0600 on
// every write; atomic replace (temp file + rename); directories 0700. A
// malformed, group/world-accessible or future-versioned store file is a typed
// error and leaves the store unusable — never silently trusted.
//
// Ownership/threading: one instance owned by the session thread; load() once,
// then const lookups and the approve/forget mutations.

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/trust/Identity.hpp>
#include <aa/trust/Trust.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aa::trust {

struct ApprovedPhone final {
    core::PhoneId phone_id{};
    TransportIdentity identity{};

    [[nodiscard]] friend bool operator==(const ApprovedPhone&, const ApprovedPhone&) noexcept =
        default;
};

// Store document bounds (fail closed beyond these).
inline constexpr std::size_t kMaxApprovedPhones = 256;
inline constexpr std::uint64_t kCurrentSchemaVersion = 2;

// ONE coherent phone-id range shared by the loader and the allocators: record
// ids live in [1, kMaxPhoneId]; next_phone_id is accepted in
// [1, kMaxPhoneId + 1] (the +1 is the post-ceilng "exhausted" state) and must
// be strictly greater than every stored record id. Allocation hands out
// next_phone_id and fails closed once it would leave the range, so a churned
// store can never contain an id its own loader rejects.
inline constexpr std::uint64_t kMaxPhoneId = 4096;

class ApprovedPhoneStore final : public TrustStore {
public:
    explicit ApprovedPhoneStore(std::filesystem::path path) : path_(std::move(path)) {}

    // `$XDG_STATE_HOME/android-auto-receiver/approved-phones.json`, falling
    // back to `$HOME/.local/state/...` when XDG_STATE_HOME is unset or empty
    // (XDG Base Directory spec). Typed error when neither is resolvable.
    [[nodiscard]] static core::Result<std::filesystem::path> default_path();

    // Load and migrate the on-disk document. A missing file is an empty store.
    // Malformed content, a foreign group/other mode, or an unknown schema
    // version is a typed error; the store then refuses every mutation and
    // fails lookup() closed (Decision::unknown).
    [[nodiscard]] core::Result<void> load();

    // TrustStore seam (session admission by opaque key).
    [[nodiscard]] Decision lookup(const PhoneIdentity& identity) const override;
    core::Result<void> approve_once(const PhoneIdentity& identity) override;
    core::Result<void> forget(const PhoneIdentity& identity) override;

    // Structured approve-once (task 27 pairing flow): persist the strongest
    // available transport identity and allocate a stable PhoneId. Approve-once
    // semantics: re-approving an already-approved key returns the existing id
    // and never adds a second record.
    [[nodiscard]] core::Result<core::PhoneId> approve(const TransportIdentity& identity);

    // ForgetPhone path: revoke by phone id, persist the removal.
    [[nodiscard]] core::Result<void> forget_phone(core::PhoneId phone);

    [[nodiscard]] const std::vector<ApprovedPhone>& phones() const noexcept { return phones_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool usable() const noexcept { return usable_; }

    // Diagnostics-safe summary (task-19): every identity appears only as a
    // per-process pseudonym — no raw serials, Bluetooth MACs or port contexts.
    [[nodiscard]] std::string redacted_summary() const;

private:
    [[nodiscard]] core::Result<void> persist();
    [[nodiscard]] std::optional<std::size_t> index_of_key(const std::string& key) const;

    std::filesystem::path path_;
    std::vector<ApprovedPhone> phones_{};
    std::uint64_t next_phone_id_{1};
    bool loaded_{false};
    bool usable_{false};
};

} // namespace aa::trust
