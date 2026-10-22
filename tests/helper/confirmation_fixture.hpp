// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Confirmation.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include "../dbus/fakes.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <unistd.h>

namespace aa::helper::test {
class Bridge final : public helper::Confirmation {
public:
    trust::ApprovedPhoneStore& store;
    int confirmations{}, cancellations{}, teardowns{}, forgets{};
    bool approved_at_call{};
    core::Result<void> result{};
    core::Result<void> ap{}, bond{};
    int ap_attempts{}, bond_attempts{};
    std::vector<helper::RevocationV1> snapshots;
    std::optional<helper::PeerIdentity> prepared;
    std::optional<std::size_t> retirement_limit;
    std::vector<std::vector<helper::RetirementSource>> retirement_batches;
    explicit Bridge(trust::ApprovedPhoneStore& approvals) : store(approvals) {}
    core::Result<void> confirmed(core::ConsumerId, core::PairingRequestId) override {
        ++confirmations;
        approved_at_call = !store.phones().empty();
        return result;
    }
    void cancel(core::ConsumerId, core::PairingRequestId) override { ++cancellations; }
    void teardown(core::ConsumerId) override { ++teardowns; }
    core::Result<std::optional<helper::PeerIdentity>> prepare(core::ConsumerId, core::PairingRequestId) override {
        return prepared;
    }
    helper::RevocationAttempt forgotten_v1(const helper::RevocationV1& snapshot,
                                          const helper::RevocationProgress& progress) override {
        ++forgets;
        snapshots.push_back(snapshot);
        EXPECT_TRUE(store.phones().empty());
        helper::RevocationAttempt report;
        report.ap = progress.ap == helper::RevocationLeg::complete ? core::Result<void>{} : (++ap_attempts, ap);
        if (helper::revocation_peer(snapshot))
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
        phones.note_candidate(trust::WirelessIdentity{"fixture", "adapter"});
        credentials.add_connection(caller, 1000);
        credentials.add_connection(rogue, 1000);
        credentials.set_owner(name, caller);
        ASSERT_TRUE(control.register_consumer(caller, name));
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
    std::size_t count_closed() const {
        return static_cast<std::size_t>(std::count_if(events.events.begin(), events.events.end(),
            [](const auto& event) { return std::holds_alternative<ipc::PairingClosed>(event); }));
    }
};
} // namespace aa::helper::test
