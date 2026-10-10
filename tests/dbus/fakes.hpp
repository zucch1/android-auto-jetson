// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test-only doubles for the task-21 control seams (peer credentials, event
// sink, pairing ids, phone directory) plus the minimal dummy consumer client
// used for the happy start/stop/state flow. The doubles are deterministic so
// the authorization matrix (different UID, same-UID unregistered, registered
// consumer, unowned name) is reproducible without a real bus; the real-bus
// cases use BusPeerLookup on a private dbus-run-session.

#include <aa/ipc/ControlService.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aa::ipc::test {

class FakePeerCredentials final : public PeerCredentialLookup {
public:
    void add_connection(const BusName& name, std::uint32_t uid) { uids_[name.value] = uid; }
    void set_owner(const BusName& name, const BusName& owner) {
        owners_[name.value] = owner.value;
        failing_.erase(name.value);
    }
    // Owner-removal double: the bus daemon reports the name as unowned.
    void remove_owner(const BusName& name) { owners_.erase(name.value); }
    // Controllable lookup-failure double: name_owner fails with a transport
    // error (distinct from the production ipc_peer_unauthorized collapse) so
    // regressions prove that ANY unverifiable lookup revokes authority.
    void fail_owner_lookup(const BusName& name) { failing_.insert(name.value); }
    void restore_owner_lookup(const BusName& name) { failing_.erase(name.value); }

    [[nodiscard]] core::Result<std::uint32_t> unix_uid(const BusName& name) const override {
        const auto found = uids_.find(name.value);
        if (found == uids_.end()) {
            return Error{ErrorCode::ipc_peer_unauthorized};
        }
        return found->second;
    }

    [[nodiscard]] core::Result<BusName> name_owner(const BusName& name) const override {
        if (failing_.contains(name.value)) {
            return Error{ErrorCode::transport_io};
        }
        const auto found = owners_.find(name.value);
        if (found == owners_.end()) {
            return Error{ErrorCode::ipc_peer_unauthorized};
        }
        return BusName{found->second};
    }

private:
    std::map<std::string, std::uint32_t> uids_{};
    std::map<std::string, std::string> owners_{};
    std::set<std::string> failing_{};
};

class RecordingEventSink final : public ControlEventSink {
public:
    void emit(const ControlEvent& event) override { events.push_back(event); }

    [[nodiscard]] std::size_t count_opened() const {
        std::size_t count = 0;
        for (const auto& event : events) {
            if (std::holds_alternative<PairingOpened>(event)) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t count_requested() const {
        std::size_t count = 0;
        for (const auto& event : events) {
            if (std::holds_alternative<PairingRequested>(event)) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::vector<core::PairingRequestId> opened_requests() const {
        std::vector<core::PairingRequestId> ids;
        for (const auto& event : events) {
            if (const auto* opened = std::get_if<PairingOpened>(&event)) {
                ids.push_back(opened->request);
            }
        }
        return ids;
    }

    std::vector<ControlEvent> events{};
};

class SequencePairingIds final : public PairingIdSource {
public:
    explicit SequencePairingIds(std::uint64_t first = 1000) : next_(first) {}

    [[nodiscard]] core::PairingRequestId next() override {
        return core::PairingRequestId{next_++};
    }

private:
    std::uint64_t next_;
};

// Adversarial id source: wraps around its period, so an unremembered id would
// be reissued. The gate's issued history must refuse every reissue.
class CyclingPairingIds final : public PairingIdSource {
public:
    explicit CyclingPairingIds(std::uint64_t period) : period_(period) {}

    [[nodiscard]] core::PairingRequestId next() override {
        return core::PairingRequestId{(next_++ % period_) + 1};
    }

private:
    std::uint64_t period_;
    std::uint64_t next_{0};
};

class RecordingPhoneDirectory final : public PhoneDirectory {
public:
    [[nodiscard]] core::Result<void> forget(core::PhoneId phone) override {
        forgotten.push_back(phone);
        return {};
    }

    std::vector<core::PhoneId> forgotten{};
};

// Minimal dummy consumer client: one connection identity driving the control
// surface exactly as a future D-Bus consumer would (register -> start ->
// status -> stop -> unregister).
class DummyConsumerClient final {
public:
    DummyConsumerClient(ControlService& service, BusName connection, BusName consumer_name)
        : service_(service), connection_(std::move(connection)),
          consumer_name_(std::move(consumer_name)) {}

    [[nodiscard]] core::Result<core::ConsumerId> register_self() {
        const auto id = service_.register_consumer(connection_, consumer_name_);
        if (id.has_value()) {
            consumer_ = id.value();
        }
        return id;
    }

    [[nodiscard]] core::Result<void> start() { return service_.start_projection(connection_); }
    [[nodiscard]] core::Result<void> stop() { return service_.stop_projection(connection_); }
    [[nodiscard]] StateSnapshot state() const { return service_.state(); }
    [[nodiscard]] core::Result<void> unregister_self() {
        return service_.unregister_consumer(connection_);
    }

    [[nodiscard]] const BusName& connection() const { return connection_; }
    [[nodiscard]] core::ConsumerId consumer() const { return consumer_; }

private:
    ControlService& service_;
    BusName connection_;
    BusName consumer_name_;
    core::ConsumerId consumer_{};
};

} // namespace aa::ipc::test
