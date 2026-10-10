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
#include <string>
#include <vector>

namespace aa::ipc::test {

class FakePeerCredentials final : public PeerCredentialLookup {
public:
    void add_connection(const BusName& name, std::uint32_t uid) { uids_[name.value] = uid; }
    void set_owner(const BusName& name, const BusName& owner) {
        owners_[name.value] = owner.value;
    }

    [[nodiscard]] core::Result<std::uint32_t> unix_uid(const BusName& name) const override {
        const auto found = uids_.find(name.value);
        if (found == uids_.end()) {
            return Error{ErrorCode::ipc_peer_unauthorized};
        }
        return found->second;
    }

    [[nodiscard]] core::Result<BusName> name_owner(const BusName& name) const override {
        const auto found = owners_.find(name.value);
        if (found == owners_.end()) {
            return Error{ErrorCode::ipc_peer_unauthorized};
        }
        return BusName{found->second};
    }

private:
    std::map<std::string, std::uint32_t> uids_{};
    std::map<std::string, std::string> owners_{};
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

class RecordingPhoneDirectory final : public PhoneDirectory {
public:
    [[nodiscard]] core::Result<void> forget(core::PhoneId phone) override {
        forgotten.push_back(phone);
        return {};
    }

    [[nodiscard]] core::Result<void> pairing_requested(core::PairingRequestId request) override {
        requested.push_back(request);
        return request_binding;
    }

    [[nodiscard]] core::Result<core::PhoneId> pairing_confirmed(
        core::PairingRequestId request) override {
        confirmed.push_back(request);
        if (!confirm_result.has_value()) {
            return confirm_result.error();
        }
        return core::PhoneId{next_phone_id++};
    }

    void pairing_cancelled(core::PairingRequestId request) override {
        cancelled.push_back(request);
    }

    [[nodiscard]] std::vector<core::PairingRequestId> reap_expired() override {
        const std::vector<core::PairingRequestId> expired = reapable;
        reapable.clear();
        return expired;
    }

    std::vector<core::PhoneId> forgotten{};
    std::vector<core::PairingRequestId> requested{};
    std::vector<core::PairingRequestId> confirmed{};
    std::vector<core::PairingRequestId> cancelled{};
    std::vector<core::PairingRequestId> reapable{};
    core::Result<void> request_binding{};
    core::Result<core::PhoneId> confirm_result{core::PhoneId{1}};
    std::uint64_t next_phone_id{1};
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
