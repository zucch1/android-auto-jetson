// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Confirmation.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include "../dbus/fakes.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <unistd.h>

namespace aa::helper::test {
class Bridge final : public helper::Confirmation {
public:
    trust::ApprovedPhoneStore& store;
    int confirmations{}, cancellations{}, teardowns{}, forgets{};
    bool approved_at_call{};
    bool expect_empty_store{true};
    core::Result<void> result{}, cancel_result{};
    core::Result<void> ap{}, bond{};
    int ap_attempts{}, bond_attempts{};
    std::vector<helper::RevocationV1> snapshots;
    std::optional<helper::PeerIdentity> prepared;
    std::optional<helper::PairingComparison> challenge;
    bool comparison_required{};
    bool requires_comparison() const override { return comparison_required; }
    bool bonded{true};
    bool fail_after_response{};
    bool approved_at_bond{};
    // Hooks run inside the helper RPC, where owner, clock and store changes may land.
    std::function<void()> on_bond_confirmed, on_acknowledge;
    int acknowledgements{};
    core::Result<void> acknowledgement{};
    bool approved_at_acknowledgement{};
    core::Result<void> bond_acknowledged(core::ConsumerId, core::PairingRequestId) override {
        ++acknowledgements;
        approved_at_acknowledgement = !store.phones().empty();
        if (on_acknowledge) on_acknowledge();
        return acknowledgement;
    }
    core::Result<bool> bond_confirmed(core::ConsumerId, core::PairingRequestId) override {
        if (on_bond_confirmed) on_bond_confirmed();
        approved_at_bond = !store.phones().empty();
        if (fail_after_response && responses) return Error{ErrorCode::helper_unavailable};
        if (!result) return result.error();
        return bonded;
    }
    int responses{};
    helper::ComparisonResponse response;
    core::ConsumerId response_generation;
    core::PairingRequestId response_request;
    core::Result<helper::PairingComparison> comparison(core::ConsumerId, core::PairingRequestId) override {
        return challenge ? core::Result<helper::PairingComparison>{*challenge} :
                           core::Result<helper::PairingComparison>{Error{ErrorCode::helper_rejected}};
    }
    core::Result<void> respond_comparison(core::ConsumerId generation, core::PairingRequestId request,
                                         helper::ComparisonResponse answer) override {
        ++responses; response = answer; response_generation = generation; response_request = request;
        challenge.reset();
        return {};
    }
    std::optional<std::size_t> retirement_limit;
    std::vector<std::vector<helper::RetirementSource>> retirement_batches;
    std::function<void()> on_forget{}, on_confirm{};
    explicit Bridge(trust::ApprovedPhoneStore& approvals) : store(approvals) {}
    core::Result<void> confirmed(core::ConsumerId, core::PairingRequestId) override {
        ++confirmations;
        if (on_confirm) on_confirm();
        approved_at_call = !store.phones().empty();
        return result;
    }
    core::Result<void> cancel(core::ConsumerId, core::PairingRequestId) override { ++cancellations; return cancel_result; }
    void teardown(core::ConsumerId) override { ++teardowns; }
    core::Result<std::optional<helper::PeerIdentity>> prepare(core::ConsumerId, core::PairingRequestId) override {
        return prepared;
    }
    helper::RevocationAttempt forgotten_v1(const helper::RevocationV1& snapshot,
                                          const helper::RevocationProgress& progress) override {
        if (on_forget) on_forget();
        ++forgets;
        snapshots.push_back(snapshot);
        if (expect_empty_store) { EXPECT_TRUE(store.phones().empty()); }
        helper::RevocationAttempt report;
        report.ap = progress.ap == helper::RevocationLeg::complete ? core::Result<void>{} : (++ap_attempts, ap);
        if ((snapshot.phone || progress.bond) && helper::revocation_peer(snapshot))
            report.bond = progress.bond == helper::RevocationLeg::complete ? core::Result<void>{} : (++bond_attempts, bond);
        retirement_batches.push_back(progress.retirements);
        report.retired = std::min(progress.retirements.size(), retirement_limit.value_or(progress.retirements.size()));
        report.retirement = report.retired == progress.retirements.size() ? core::Result<void>{} :
                            core::Result<void>{Error{ErrorCode::helper_unavailable}};
        return report;
    }
};
class HelperConfirmation : public ::testing::Test {
protected:
    std::filesystem::path directory = std::filesystem::current_path() / ("bridge-" + std::to_string(::getpid()));
    trust::ApprovedPhoneStore store{directory / "store"};
    trust::SteadyClock clock;
    trust::PairingPolicy phones{store, clock};
    ipc::test::FakePeerCredentials credentials;
    ipc::test::RecordingEventSink events;
    ipc::test::SequencePairingIds ids;
    Bridge bridge{store};
    ipc::ControlService control{{1000}, {credentials, events, ids, phones, &bridge}};
    ipc::BusName caller{":1.2"}, rogue{":1.3"}, name{"org.custom.Consumer"};
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        ASSERT_TRUE(store.load());
        phones.note_candidate(trust::WirelessIdentity{"02:00:00:00:00:01", "adapter"});
        credentials.add_connection(caller, 1000);
        credentials.add_connection(rogue, 1000);
        credentials.set_owner(name, caller);
        ASSERT_TRUE(control.register_consumer(caller, name));
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
    bool has_diagnostic(std::string_view code) const {
        return std::any_of(events.events.begin(), events.events.end(), [code](const auto& event) {
            const auto* diagnostic = std::get_if<ipc::DiagnosticEmitted>(&event);
            return diagnostic && diagnostic->event == code;
        });
    }
    std::size_t count_closed() const {
        return static_cast<std::size_t>(std::count_if(events.events.begin(), events.events.end(),
            [](const auto& event) { return std::holds_alternative<ipc::PairingClosed>(event); }));
    }
};
} // namespace aa::helper::test
