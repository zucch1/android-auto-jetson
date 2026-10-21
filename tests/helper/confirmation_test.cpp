// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/Confirmation.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include "../dbus/fakes.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

namespace {
using namespace aa;
class Bridge final : public helper::Confirmation {
public:
    trust::ApprovedPhoneStore& store;
    int confirmations{}, cancellations{}, teardowns{}, forgets{};
    bool approved_at_call{};
    core::Result<void> result{};
    explicit Bridge(trust::ApprovedPhoneStore& approvals) : store(approvals) {}
    core::Result<void> confirmed(core::ConsumerId, core::PairingRequestId) override {
        ++confirmations;
        approved_at_call = !store.phones().empty();
        return result;
    }
    void cancel(core::ConsumerId, core::PairingRequestId) override { ++cancellations; }
    void teardown(core::ConsumerId) override { ++teardowns; }
    void forgotten() override { ++forgets; }
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
};
TEST_F(HelperConfirmation, HelperFailureDoesNotClaimTrustRollbackOrPairingOpened) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    bridge.result = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    EXPECT_EQ(bridge.confirmations, 1);
    EXPECT_TRUE(bridge.approved_at_call);
    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_FALSE(control.pairing_window_open());
    EXPECT_EQ(events.count_opened(), 0U);
    EXPECT_EQ(bridge.cancellations, 1);
}
TEST_F(HelperConfirmation, UnauthorizedAndFailedTrustNeverReachBridge) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    EXPECT_FALSE(control.confirm_phone_pairing(rogue, source.value()));
    EXPECT_EQ(bridge.confirmations, 0);
    ASSERT_TRUE(control.cancel_phone_pairing(caller, source.value()));
    EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    EXPECT_EQ(bridge.confirmations, 0);
}
TEST_F(HelperConfirmation, ShutdownAndOwnerLossNotifyLifecycle) {
    credentials.remove_owner(name);
    control.refresh_consumer_owner();
    EXPECT_EQ(bridge.teardowns, 1);
    control.shutdown();
    EXPECT_EQ(bridge.teardowns, 1);
    credentials.set_owner(name, caller);
    ASSERT_TRUE(control.register_consumer(caller, name));
    control.shutdown();
    EXPECT_EQ(bridge.teardowns, 2);
}
}
