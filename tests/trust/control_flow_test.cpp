// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 control-flow integration (QA: "unauthorized AddPhone (wrong consumer
// / unregistered caller) is rejected and does NOT modify the store", and the
// approve-once flow driven by the same one-time request id): the store only
// changes when the registered active-session consumer confirms its own
// request, and forget rides the PhoneDirectory seam.
#include <aa/ipc/ControlService.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <aa/trust/Store.hpp>

#include "../dbus/fakes.hpp"
#include "fakes.hpp"

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
using aa::ErrorCode;
using aa::ipc::BusName;
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;

constexpr std::uint32_t kSessionUid = 1000;

struct Fixture final {
    test::TempStoreDir dir{};
    trust::ApprovedPhoneStore store{dir.file()};
    test::ManualClock clock{};
    trust::PairingPolicy policy{store, clock};
    aa::ipc::test::FakePeerCredentials credentials{};
    aa::ipc::test::RecordingEventSink events{};
    aa::ipc::test::SequencePairingIds ids{};
    ControlService service;

    Fixture()
        : service(ControlServiceConfig{kSessionUid},
                  ControlServiceDeps{credentials, events, ids, policy}) {
        static_cast<void>(store.load());
    }
};

TEST(ControlFlow, ApproveOnceThenReconnectAndForget) {
    Fixture f;
    const BusName connection{":1.10"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(f.service.register_consumer(connection, consumer_name).has_value());

    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    const auto request = f.service.request_add_phone(connection);
    ASSERT_TRUE(request.has_value());

    const auto before = f.store.lookup(test::identity_of(trust::TransportIdentity{test::kWired}));
    EXPECT_EQ(before, trust::Decision::unknown);

    ASSERT_TRUE(f.service.confirm_phone_pairing(connection, request.value()).has_value());
    const auto identity = test::identity_of(trust::TransportIdentity{test::kWired});
    EXPECT_EQ(f.store.lookup(identity), trust::Decision::approved);

    const auto& phones = f.store.phones();
    ASSERT_EQ(phones.size(), 1U);
    ASSERT_TRUE(f.service.forget_phone(connection, phones[0].phone_id).has_value());
    EXPECT_EQ(f.store.lookup(identity), trust::Decision::unknown);
}

TEST(ControlFlow, UnregisteredCallerAddPhoneLeavesStoreUnmodified) {
    Fixture f;
    const BusName connection{":1.11"};
    f.credentials.add_connection(connection, kSessionUid);

    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    const auto denied = f.service.request_add_phone(connection);

    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
}

TEST(ControlFlow, WrongUidCallerAddPhoneLeavesStoreUnmodified) {
    Fixture f;
    const BusName connection{":1.12"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(f.service.register_consumer(connection, consumer_name).has_value());

    const BusName intruder{":1.13"};
    f.credentials.add_connection(intruder, kSessionUid + 1);
    f.credentials.set_owner(consumer_name, intruder);
    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    const auto denied = f.service.request_add_phone(intruder);

    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_peer_unauthorized);
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
}

TEST(ControlFlow, UnownedConsumerNameAddPhoneLeavesStoreUnmodified) {
    Fixture f;
    const BusName connection{":1.14"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, BusName{":1.99"});
    const auto registered = f.service.register_consumer(connection, consumer_name);
    ASSERT_FALSE(registered.has_value());

    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    const auto denied = f.service.request_add_phone(connection);

    ASSERT_FALSE(denied.has_value());
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
}

TEST(ControlFlow, RequestWithoutCandidateIsRejectedAndStoreUnmodified) {
    Fixture f;
    const BusName connection{":1.15"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(f.service.register_consumer(connection, consumer_name).has_value());

    const auto denied = f.service.request_add_phone(connection);

    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
}

TEST(ControlFlow, ExpiredConfirmIsRejectedAndNotPersisted) {
    Fixture f;
    const BusName connection{":1.16"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(f.service.register_consumer(connection, consumer_name).has_value());

    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    const auto request = f.service.request_add_phone(connection);
    ASSERT_TRUE(request.has_value());

    f.clock.advance(aa::core::Milliseconds{
        trust::PairingPolicy::kConfirmWindow.count + 1});
    const auto expired = f.service.confirm_phone_pairing(connection, request.value());

    ASSERT_FALSE(expired.has_value());
    EXPECT_EQ(expired.error().code(), ErrorCode::session_pairing_timeout);
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
    EXPECT_FALSE(f.service.pairing_window_open());
}

TEST(ControlFlow, CancelledPairingLeavesStoreUnmodified) {
    Fixture f;
    const BusName connection{":1.17"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(f.service.register_consumer(connection, consumer_name).has_value());

    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    const auto request = f.service.request_add_phone(connection);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.service.cancel_phone_pairing(connection, request.value()).has_value());

    const auto late = f.service.confirm_phone_pairing(connection, request.value());
    ASSERT_FALSE(late.has_value());
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
}

TEST(ControlFlow, UnregisterClosesPendingWithoutTouchingStore) {
    Fixture f;
    const BusName connection{":1.18"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(f.service.register_consumer(connection, consumer_name).has_value());

    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(f.service.request_add_phone(connection).has_value());
    ASSERT_TRUE(f.service.unregister_consumer(connection).has_value());

    EXPECT_EQ(f.policy.pending_count(), 0U);
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
}

TEST(ControlFlow, ExpiredRequestsReleaseGateCapacity) {
    Fixture f;
    const BusName connection{":1.19"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    f.credentials.add_connection(connection, kSessionUid);
    f.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(f.service.register_consumer(connection, consumer_name).has_value());

    f.policy.note_candidate(trust::TransportIdentity{test::kWired});
    std::vector<aa::core::PairingRequestId> expired;
    for (std::size_t index = 0; index < trust::PairingPolicy::kMaxPending; ++index) {
        const auto request = f.service.request_add_phone(connection);
        ASSERT_TRUE(request.has_value());
        expired.push_back(request.value());
    }
    const auto blocked = f.service.request_add_phone(connection);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error().code(), ErrorCode::ipc_queue_full);

    f.clock.advance(aa::core::Milliseconds{
        trust::PairingPolicy::kConfirmWindow.count + 1});
    f.policy.note_candidate(trust::TransportIdentity{test::kWireless});
    const auto fresh = f.service.request_add_phone(connection);
    ASSERT_TRUE(fresh.has_value());

    std::vector<aa::core::PairingRequestId> closed;
    for (const auto& event : f.events.events) {
        if (const auto* pair = std::get_if<aa::ipc::PairingClosed>(&event)) {
            closed.push_back(pair->request);
        }
    }
    std::sort(expired.begin(), expired.end(),
              [](aa::core::PairingRequestId left, aa::core::PairingRequestId right) {
                  return left.value < right.value;
              });
    std::sort(closed.begin(), closed.end(),
              [](aa::core::PairingRequestId left, aa::core::PairingRequestId right) {
                  return left.value < right.value;
              });
    EXPECT_EQ(closed, expired);  // exactly one PairingClosed per expired id
    EXPECT_EQ(f.policy.pending_count(), 1U);
    EXPECT_TRUE(f.store.phones().empty());

    for (const auto request : expired) {
        const auto confirm = f.service.confirm_phone_pairing(connection, request);
        ASSERT_FALSE(confirm.has_value());
    }
    EXPECT_TRUE(f.store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(f.dir.file()));
}

} // namespace
