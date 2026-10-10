// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/ipc/PeerCredentials.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace aa::ipc {

// One-time pairing request-id source (task 21). Production ids must be
// unpredictable so a D-Bus eavesdropper cannot pre-claim a pairing window;
// tests inject a deterministic source.
class PairingIdSource {
public:
    virtual ~PairingIdSource() = default;
    PairingIdSource() = default;
    PairingIdSource(const PairingIdSource&) = delete;
    PairingIdSource& operator=(const PairingIdSource&) = delete;
    PairingIdSource(PairingIdSource&&) = delete;
    PairingIdSource& operator=(PairingIdSource&&) = delete;

    // Never returns the unset id.
    [[nodiscard]] virtual core::PairingRequestId next() = 0;
};

// Production source: 64 unpredictable bits per id (std::random_device, the
// platform CSPRNG-backed source), with the unset id rejected by construction.
class RandomPairingIdSource final : public PairingIdSource {
public:
    [[nodiscard]] core::PairingRequestId next() override;
};

// The consumer a pairing request is bound to: exactly the registered consumer
// (id AND owned bus name) that must confirm it.
struct BoundConsumer final {
    core::ConsumerId consumer{};
    BusName name{};

    [[nodiscard]] friend bool operator==(const BoundConsumer&, const BoundConsumer&) noexcept =
        default;
};

// One-time request-id pairing gate (task 21). RequestAddPhone allocates a
// pending id bound to one consumer; the pairing window opens ONLY when that
// same consumer confirms that same id. Ids are one-time FOR THE LIFETIME of
// the gate: every issued id is remembered until restart, so a cycling or
// compromised id source can never re-introduce a spent id and let a stale
// ConfirmPhonePairing open a new window. The admission budget is explicitly
// bounded (kIssuedBudget issued ids per service lifetime); when it is
// exhausted the gate fails closed (no new requests until restart).
//
// Error contract (typed, fail-closed):
//   - unknown id            -> ipc_malformed_request
//   - already issued id     -> ipc_consumer_rejected (spent or duplicate)
//   - wrong consumer        -> ipc_peer_unauthorized
//   - window already open   -> ipc_consumer_rejected
//   - pending table full    -> ipc_queue_full
//   - issued budget spent   -> ipc_queue_full (fail closed until restart)
//
// The pairing-window machinery behind the gate (trust store, timing policy)
// is todo 27 and hooks window_open()/open_request(); nothing else here opens
// a window.
class PairingGate final {
public:
    explicit PairingGate(PairingIdSource& ids) : ids_(ids) {}

    [[nodiscard]] core::Result<core::PairingRequestId> request(const BoundConsumer& consumer);
    [[nodiscard]] core::Result<void> confirm(const BoundConsumer& consumer,
                                             core::PairingRequestId request);
    [[nodiscard]] core::Result<void> cancel(const BoundConsumer& consumer,
                                            core::PairingRequestId request);

    // Drops every pending request and closes an open window; returns the ids
    // that just closed so the caller can emit PairingClosed for each. Issued
    // ids stay remembered (one-time for the gate lifetime).
    [[nodiscard]] std::vector<core::PairingRequestId> close_all();

    [[nodiscard]] bool window_open() const noexcept { return open_.has_value(); }
    [[nodiscard]] std::optional<core::PairingRequestId> open_request() const { return open_; }

    static constexpr std::size_t kMaxPending = 8;
    static constexpr std::size_t kIssuedBudget = 64;

private:
    struct Entry final {
        core::PairingRequestId request{};
        BoundConsumer consumer{};
        bool open{false};
    };

    [[nodiscard]] bool issued(core::PairingRequestId request) const noexcept;

    PairingIdSource& ids_;
    std::array<Entry, kMaxPending> entries_{};
    std::size_t count_{0};
    std::array<core::PairingRequestId, kIssuedBudget> issued_{};
    std::size_t issued_count_{0};
    std::optional<core::PairingRequestId> open_{};
};

} // namespace aa::ipc
