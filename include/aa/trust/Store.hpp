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
// spoofed by anyone with physical access to a receiver USB port; an approved
// serial-backed wired record therefore authorizes "the approved identifiers
// presented on any receiver port" (matching is port-independent, #133; see
// match_key() in Identity.hpp), not "this specific handset". A port-only
// (serial-less) record still matches only on the port it was approved on. Wireless records bind the BlueZ bond
// (stronger, still bond-not-human identity). Unknown phones always fail closed:
// lookup() returns Decision::unknown and allows_session() is false.
//
// Persistence: `$XDG_STATE_HOME/android-auto-receiver/approved-phones.json`
// (XDG fallback `~/.local/state/...`), created 0600 and re-enforced to 0600 on
// every write; atomic replace (temp file + rename); directories 0700. A
// malformed, group/world-accessible or future-versioned store file is a typed
// error and leaves the store unusable — never silently trusted.
//
// PARTIAL-EFFECT CONTRACT: a mutation that returns
// ErrorCode::trust_store_not_durable (703) IS already applied in memory and
// on disk (the replacement renamed; only crash durability of the new
// directory entry is unconfirmed) — an approval returning 703 may already
// exist and a forget returning 703 is already revoked. Every other mutation
// error is pre-replacement and leaves the store fully unchanged (in memory
// and on disk).
//
// Ownership/threading: one instance owned by the session thread; load() once,
// then const lookups and the approve/forget mutations.

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/trust/Identity.hpp>
#include <aa/trust/Trust.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aa::trust {

struct ApprovedPhone final {
    core::PhoneId phone_id{};
    TransportIdentity identity{};
    IdentityStrength strength{IdentityStrength::opaque};

    [[nodiscard]] friend bool operator==(const ApprovedPhone&, const ApprovedPhone&) noexcept =
        default;
};

// Store document bounds (fail closed beyond these).
inline constexpr std::size_t kMaxApprovedPhones = 256;
inline constexpr std::uint64_t kCurrentSchemaVersion = 3;

// ONE coherent phone-id range shared by the loader and the allocators: record
// ids live in [1, kMaxPhoneId]; next_phone_id is accepted in
// [1, kMaxPhoneId + 1] (the +1 is the post-ceilng "exhausted" state) and must
// be strictly greater than every stored record id. Allocation hands out
// next_phone_id and fails closed once it would leave the range, so a churned
// store can never contain an id its own loader rejects.
inline constexpr std::uint64_t kMaxPhoneId = 4096;

// A revocation split so its durable write can run off the store owner (#90:
// the owner is the session thread, which must never wait on disk). The owner
// prepares (no mutation), any thread writes (file I/O only, never the store
// object), and the owner applies the result. In-memory trust changes only once
// the replacement landed, exactly as with the synchronous forget_phone().
// Orders every durable replacement of one store file (#90 review 4). Each
// write takes a strictly increasing generation when its document is built; a
// write whose generation is not newer than the last committed one is discarded
// before it touches the file, so a stalled (or abandoned) older snapshot can
// never rename over a newer revocation. Replacements are serialized under the
// mutex, which only persistence threads hold for long; the owner's synchronous
// persist() waits for it only up to kOwnerWriteWait and otherwise fails closed.
struct StoreWriteSequencer {
    std::timed_mutex mutex;
    std::atomic<std::uint64_t> next{1};
    std::uint64_t committed{0}; // guarded by mutex
};

struct PreparedForget {
    core::PhoneId phone{};
    std::string match_key;
    std::string document;
    std::filesystem::path path;
    std::uint64_t mutation_epoch{};
    std::uint64_t generation{};
    std::shared_ptr<StoreWriteSequencer> sequencer;
};

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
    std::uint64_t reload_epoch() const { return reload_epoch_; }

    // TrustStore seam (session admission by opaque key). lookup/forget match
    // by match_key(): a serial-backed wired key matches its phone on any port. approve_once rejects
    // all opaque w1: keys; wired approvals must use the structured APIs.
    [[nodiscard]] Decision lookup(const PhoneIdentity& identity) const override;
    core::Result<void> approve_once(const PhoneIdentity& identity) override;
    core::Result<void> forget(const PhoneIdentity& identity) override;

    // Structured approve-once (task 27 pairing flow): persist the strongest
    // available transport identity and allocate a stable PhoneId. Approve-once
    // semantics: re-approving an already-approved key (by match_key, so the
    // same serial-backed phone on another port) returns the existing id and
    // never adds a second record. Serial-less wired identities are rejected
    // with trust_port_only_requires_opt_in; use approve_port_only explicitly.
    [[nodiscard]] core::Result<core::PhoneId> approve(const TransportIdentity& identity);

    // Explicit weaker grant: any device presenting the same descriptors on this
    // port will match (on this port ONLY; never widened to other ports). Normal pairing never calls this API.
    [[nodiscard]] core::Result<core::PhoneId> approve_port_only(const WiredIdentity& identity);

    // ForgetPhone path: revoke by phone id, persist the removal.
    // Equivalent to apply_forget(p, write_prepared(p)) for p = prepare_forget(phone).
    [[nodiscard]] core::Result<void> forget_phone(core::PhoneId phone);

    // Two-phase ForgetPhone (see PreparedForget). prepare_forget() and
    // apply_forget() run on the owner; write_prepared() touches only the file.
    [[nodiscard]] core::Result<PreparedForget> prepare_forget(core::PhoneId phone) const;
    [[nodiscard]] static core::Result<void> write_prepared(const PreparedForget& prepared);
    // write_prepared() discards a superseded document (an older generation than
    // the last committed write) with `cancelled` and never touches the file.
    // `written` is write_prepared()'s result: success or trust_store_not_durable
    // (committed) removes the phone in memory; any other error changes nothing.
    // A prepared revocation is superseded() when another mutation landed after
    // it was prepared: its document is stale, so apply_forget() changes nothing
    // and answers `cancelled`; the caller prepares and writes again.
    [[nodiscard]] core::Result<void> apply_forget(const PreparedForget& prepared,
                                                  core::Result<void> written);
    [[nodiscard]] bool superseded(const PreparedForget& prepared) const noexcept {
        return prepared.mutation_epoch != mutation_epoch_;
    }
    static constexpr std::chrono::milliseconds kOwnerWriteWait{2000};

    // Retry the crash-durability half of a mutation that returned
    // trust_store_not_durable: sync the store's containing directory again.
    // Success means the committed replacement is now durable; failure is
    // trust_store_not_durable (trust_store_io when the store is unusable).
    // Changes nothing in memory or on disk.
    [[nodiscard]] core::Result<void> sync_directory() const;

    [[nodiscard]] const std::vector<ApprovedPhone>& phones() const noexcept { return phones_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool usable() const noexcept { return usable_; }

    // Number of unconsented port-only records dropped by the last successful
    // load's v1/v2 migration, including malformed reserved w1: keys dropped
    // fail-closed; zero after an empty/v3 load or a failed load.
    [[nodiscard]] std::size_t dropped_port_only_count() const noexcept {
        return dropped_port_only_count_;
    }

    // Diagnostics-safe summary (task-19): every identity appears only as a
    // per-process pseudonym — no raw serials, Bluetooth MACs or port contexts.
    [[nodiscard]] std::string redacted_summary() const;

private:
    [[nodiscard]] core::Result<core::PhoneId> approve_impl(const TransportIdentity& identity,
                                                        bool allow_port_only);
    [[nodiscard]] core::Result<void> persist();
    [[nodiscard]] std::optional<std::size_t> index_of_key(const std::string& key) const;

    std::filesystem::path path_;
    std::vector<ApprovedPhone> phones_{};
    std::uint64_t next_phone_id_{1};
    std::size_t dropped_port_only_count_{0};
    bool loaded_{false};
    std::uint64_t reload_epoch_{};
    bool usable_{false};
    std::uint64_t mutation_epoch_{}; // bumped by every in-memory mutation
    std::shared_ptr<StoreWriteSequencer> sequencer_ = std::make_shared<StoreWriteSequencer>();
};

// Consumer-safe redacted approved-phone summary for D-Bus ListPhones (contract
// 1.2.0): opaque phone_id (decimal uint64 matching ForgetPhone), per-process
// pseudonymized label, transport ("wired", "wireless", or "opaque"), and
// strength. Never exposes raw serials, MACs, ports, or legacy keys. Fails
// closed with trust_store_io when the store is unusable.
[[nodiscard]] core::Result<std::vector<std::map<std::string, std::string>>> list_approved_phones(
    const ApprovedPhoneStore& store);

} // namespace aa::trust
