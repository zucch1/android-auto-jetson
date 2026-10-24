// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WirelessReceiver.hpp>
#include "../dbus/fakes.hpp"
#include <gtest/gtest.h>

namespace {
struct Confirmation final : aa::helper::Confirmation {
    unsigned teardowns{};
    aa::core::ConsumerId retired{};
    aa::core::Result<void> confirmed(aa::core::ConsumerId, aa::core::PairingRequestId) override { return {}; }
    void cancel(aa::core::ConsumerId, aa::core::PairingRequestId) override {}
    void teardown(aa::core::ConsumerId generation) override { ++teardowns; retired = generation; }
    aa::helper::RevocationAttempt forgotten_v1(const aa::helper::RevocationV1&,
                                               const aa::helper::RevocationProgress&) override { return {}; }
};
TEST(WirelessControl, ActualControlServiceRevokesRegistrationButDefersActiveSourceCleanup) {
    aa::ipc::test::FakePeerCredentials credentials;
    aa::ipc::test::RecordingEventSink events;
    aa::ipc::test::SequencePairingIds ids;
    aa::ipc::test::RecordingPhoneDirectory phones;
    Confirmation helper;
    bool active = true, admission = true;
    aa::app::WirelessConfirmation confirmation{helper, [&] { return active; },
        [&](aa::core::ConsumerId) { admission = false; }};
    aa::ipc::ControlService control{{1000}, {credentials, events, ids, phones, &confirmation}};
    const aa::ipc::BusName connection{":1.10"}, name{"org.custom.ConsumerA"};
    credentials.add_connection(connection, 1000);
    credentials.set_owner(name, connection);
    auto generation = control.register_consumer(connection, name);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(control.start_projection(connection));
    credentials.remove_owner(name);
    EXPECT_EQ(control.state().consumer.value, 0U);
    EXPECT_FALSE(admission);
    EXPECT_FALSE(control.start_projection(connection));
    EXPECT_EQ(helper.teardowns, 0U);
    confirmation.flush();
    EXPECT_EQ(helper.teardowns, 0U);
    EXPECT_FALSE(confirmation.confirmed(aa::core::ConsumerId{2}, aa::core::PairingRequestId{2}));
    active = false;
    confirmation.flush();
    EXPECT_EQ(helper.teardowns, 1U);
    EXPECT_EQ(helper.retired, generation.value());
    confirmation.flush();
    EXPECT_EQ(helper.teardowns, 1U);
}
TEST(WirelessControl, AdmissionLossDoesNotDeferCleanup) {
    Confirmation helper;
    bool admission = true;
    aa::app::WirelessConfirmation confirmation{helper, [] { return false; },
        [&](aa::core::ConsumerId) { admission = false; }};
    confirmation.teardown(aa::core::ConsumerId{7});
    EXPECT_FALSE(admission);
    EXPECT_EQ(helper.teardowns, 1U);
}
}
