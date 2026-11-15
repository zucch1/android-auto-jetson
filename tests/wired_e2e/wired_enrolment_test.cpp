// SPDX-License-Identifier: GPL-3.0-or-later
// #133 wired enrolment through the authenticated control contract: a phone
// admission refused can be approved only by the registered consumer's
// one-time, expiring request bound to that refused attempt's captured
// identity, after the candidate was presented for an explicit yes/no, and the
// yes ends in a durable store write. Everything else writes nothing.
#include <aa/app/WiredEnrolment.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/transport/UsbAoa.hpp>
#include <aa/trust/Store.hpp>

#include "../dbus/fakes.hpp"
#include "../trust/fakes.hpp"
#include "../trust/syscall_faults.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace ttest = aa::trust::test;
using aa::ErrorCode;
using aa::ipc::BusName;

constexpr std::uint32_t kSessionUid = 1000;

const trust::WiredIdentity kPhone{"04e8", "6860", "R5CXFIXTURE01", "samsung", "SM-S928U",
                                  "R5CXFIXTURE01", "1-2.3"};
const trust::WiredIdentity kOther{"04e8", "6860", "R5CXFIXTURE02", "samsung", "SM-S928U",
                                  "R5CXFIXTURE02", "1-2.3"};

trust::WiredIdentity on_port(trust::WiredIdentity identity, std::string port) {
    identity.port_context = std::move(port);
    return identity;
}

struct Fixture final {
    ttest::TempStoreDir dir{};
    trust::ApprovedPhoneStore store{dir.file()};
    ttest::ManualClock clock{};
    aa::ipc::test::RecordingPhoneDirectory after{};
    aa::app::WiredEnrolment enrolment{store, clock, after};
    aa::ipc::test::FakePeerCredentials credentials{};
    aa::ipc::test::RecordingEventSink events{};
    aa::ipc::test::SequencePairingIds ids{};
    aa::ipc::ControlService service{aa::ipc::ControlServiceConfig{kSessionUid},
                                    aa::ipc::ControlServiceDeps{credentials, events, ids, enrolment}};
    const BusName consumer{":1.40"};
    const BusName consumer_name{"org.custom.Infotainment"};

    Fixture() {
        EXPECT_TRUE(store.load().has_value());
        credentials.add_connection(consumer, kSessionUid);
        credentials.set_owner(consumer_name, consumer);
        EXPECT_TRUE(service.register_consumer(consumer, consumer_name).has_value());
    }

    // The receiver's refusal path: admission (the real admit_via seam) refuses
    // the captured identity, and only then is it noted as the candidate.
    bool refuse(const trust::WiredIdentity& identity) {
        if (aa::transport::admit_via(store)(identity)) return false;
        return enrolment.note_refused(identity).has_value();
    }
    [[nodiscard]] bool admitted(const trust::WiredIdentity& identity) const {
        return aa::transport::admit_via(store)(identity);
    }
    [[nodiscard]] std::size_t closed(aa::core::PairingRequestId request) const {
        return static_cast<std::size_t>(std::count_if(events.events.begin(), events.events.end(),
            [request](const auto& event) {
                const auto* closed = std::get_if<aa::ipc::PairingClosed>(&event);
                return closed != nullptr && closed->request == request;
            }));
    }
    void expect_store_untouched() const {
        EXPECT_TRUE(store.phones().empty());
        EXPECT_FALSE(std::filesystem::exists(dir.file()));
    }
};

TEST(WiredEnrolment, RefusedThenRequestThenExplicitYesIsAdmittedDurably) {
    Fixture f;
    ASSERT_FALSE(f.admitted(kPhone));
    ASSERT_TRUE(f.refuse(kPhone));

    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(f.events.count_requested(), 1U);
    EXPECT_FALSE(f.admitted(kPhone));  // a request alone approves nothing

    const auto candidate = f.service.pairing_candidate(f.consumer, request.value());
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate.value().at("transport"), "wired");
    EXPECT_EQ(candidate.value().at("strength"), "serial-backed");
    EXPECT_EQ(candidate.value().at("label"), trust::redacted_label(trust::TransportIdentity{kPhone}));
    for (const auto& [name, value] : candidate.value()) {
        EXPECT_EQ(value.find("R5CXFIXTURE01"), std::string::npos) << name;
        EXPECT_EQ(value.find("1-2.3"), std::string::npos) << name;
    }

    ASSERT_FALSE(f.enrolment.completion().has_value());
    ASSERT_TRUE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
    ASSERT_TRUE(f.enrolment.completion().has_value());
    EXPECT_TRUE(f.enrolment.completion()->durable);
    EXPECT_TRUE(f.admitted(kPhone));
    EXPECT_TRUE(f.admitted(on_port(kPhone, "1-2.1")));  // #133: any port
    EXPECT_FALSE(f.admitted(kOther));

    // Durable: a fresh load of the same file admits the phone.
    trust::ApprovedPhoneStore fresh{f.dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_TRUE(aa::transport::admit_via(fresh)(kPhone));

    // One-time: the spent id confirms nothing again.
    EXPECT_FALSE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
    ASSERT_TRUE(f.service.cancel_phone_pairing(f.consumer, request.value()).has_value());
    EXPECT_EQ(f.store.phones().size(), 1U);
}

// #141 B2: a rename that landed with a failed directory sync is never a
// durable-success reply. The committed record is kept; the bounded retry
// fails too, so ConfirmPhonePairing reports trust_store_not_durable, the
// request closes without PairingOpened, and completion() says not durable.
TEST(WiredEnrolment, DirectorySyncFailureIsReportedNotDurable) {
    Fixture f;
    // An existing private store directory: the only directory sync is the
    // one after the rename (no fsync of a newly created ancestor).
    std::filesystem::create_directories(f.dir.file().parent_path());
    std::filesystem::permissions(f.dir.file().parent_path(), std::filesystem::perms::owner_all);
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.service.pairing_candidate(f.consumer, request.value()).has_value());
    {
        const ttest::faults::ScopedFault fault{ttest::faults::Fault::dir_fsync};
        const auto confirmed = f.service.confirm_phone_pairing(f.consumer, request.value());
        ASSERT_FALSE(confirmed.has_value());
        EXPECT_EQ(confirmed.error().code(), ErrorCode::trust_store_not_durable);
    }
    EXPECT_EQ(f.events.count_opened(), 0U);
    EXPECT_EQ(f.closed(request.value()), 1U);
    ASSERT_TRUE(f.enrolment.completion().has_value());
    EXPECT_FALSE(f.enrolment.completion()->durable);
    // Committed, not rolled back: memory matches the renamed file.
    ASSERT_EQ(f.store.phones().size(), 1U);
    EXPECT_EQ(f.enrolment.completion()->phone, f.store.phones()[0].phone_id);
    // The spent id approves nothing more and cannot be turned into a success.
    EXPECT_FALSE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
}

TEST(WiredEnrolment, TransientDirectorySyncFailureIsRetriedToDurableSuccess) {
    Fixture f;
    std::filesystem::create_directories(f.dir.file().parent_path());
    std::filesystem::permissions(f.dir.file().parent_path(), std::filesystem::perms::owner_all);
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.service.pairing_candidate(f.consumer, request.value()).has_value());
    {
        const ttest::faults::ScopedFault fault{ttest::faults::Fault::dir_fsync_eio_once};
        ASSERT_TRUE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
    }
    EXPECT_EQ(f.events.count_opened(), 1U);
    ASSERT_TRUE(f.enrolment.completion().has_value());
    EXPECT_TRUE(f.enrolment.completion()->durable);
    trust::ApprovedPhoneStore fresh{f.dir.file()};
    ASSERT_TRUE(fresh.load().has_value());
    EXPECT_TRUE(aa::transport::admit_via(fresh)(kPhone));
}

TEST(WiredEnrolment, ExplicitNoWritesNothing) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.service.pairing_candidate(f.consumer, request.value()).has_value());
    ASSERT_TRUE(f.service.cancel_phone_pairing(f.consumer, request.value()).has_value());
    EXPECT_EQ(f.closed(request.value()), 1U);
    EXPECT_FALSE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
    EXPECT_FALSE(f.admitted(kPhone));
    f.expect_store_untouched();
    EXPECT_EQ(f.enrolment.pending_count(), 0U);
}

TEST(WiredEnrolment, ConfirmOfAnUnpresentedCandidateIsRefusedAndCloses) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    const auto confirmed = f.service.confirm_phone_pairing(f.consumer, request.value());
    ASSERT_FALSE(confirmed.has_value());
    EXPECT_EQ(confirmed.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(f.closed(request.value()), 1U);
    EXPECT_FALSE(f.service.pairing_candidate(f.consumer, request.value()).has_value());
    f.expect_store_untouched();
}

TEST(WiredEnrolment, ExpiredRequestWritesNothing) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.service.pairing_candidate(f.consumer, request.value()).has_value());
    f.clock.advance(aa::core::Milliseconds{trust::PairingPolicy::kConfirmWindow.count + 1});
    // The control service reaps the expired request on its next call (or idle
    // refresh), closes it once, and the late confirm meets a spent id.
    const auto late = f.service.confirm_phone_pairing(f.consumer, request.value());
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_EQ(f.closed(request.value()), 1U);
    EXPECT_FALSE(f.admitted(kPhone));
    f.expect_store_untouched();
}

TEST(WiredEnrolment, ExpiredRequestPresentsNothing) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    f.clock.advance(aa::core::Milliseconds{trust::PairingPolicy::kConfirmWindow.count + 1});
    const auto candidate = f.service.pairing_candidate(f.consumer, request.value());
    ASSERT_FALSE(candidate.has_value());
    EXPECT_EQ(candidate.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_EQ(f.closed(request.value()), 1U);
    EXPECT_EQ(f.enrolment.pending_count(), 0U);
    f.expect_store_untouched();
}

TEST(WiredEnrolment, ApprovalIsBoundToTheRefusedSnapshotNotALaterIdentity) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.refuse(kOther));  // a different phone is refused afterwards
    const auto candidate = f.service.pairing_candidate(f.consumer, request.value());
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate.value().at("label"), trust::redacted_label(trust::TransportIdentity{kPhone}));
    ASSERT_TRUE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
    EXPECT_TRUE(f.admitted(kPhone));
    EXPECT_FALSE(f.admitted(kOther));
    ASSERT_EQ(f.store.phones().size(), 1U);
}

TEST(WiredEnrolment, UnauthenticatedCallersAreRefusedAndWriteNothing) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    const BusName stranger{":1.41"};
    f.credentials.add_connection(stranger, kSessionUid + 1);
    const BusName same_uid{":1.42"};
    f.credentials.add_connection(same_uid, kSessionUid);

    const auto foreign_request = f.service.request_add_phone(stranger);
    ASSERT_FALSE(foreign_request.has_value());
    EXPECT_EQ(foreign_request.error().code(), ErrorCode::ipc_peer_unauthorized);
    const auto unowned_request = f.service.request_add_phone(same_uid);
    ASSERT_FALSE(unowned_request.has_value());

    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    for (const BusName& intruder : {stranger, same_uid}) {
        EXPECT_FALSE(f.service.pairing_candidate(intruder, request.value()).has_value());
        EXPECT_FALSE(f.service.confirm_phone_pairing(intruder, request.value()).has_value());
    }
    EXPECT_FALSE(f.admitted(kPhone));
    f.expect_store_untouched();

    // An unregistered service (no consumer at all) refuses outright.
    ASSERT_TRUE(f.service.unregister_consumer(f.consumer).has_value());
    const auto unregistered = f.service.request_add_phone(f.consumer);
    ASSERT_FALSE(unregistered.has_value());
    EXPECT_EQ(unregistered.error().code(), ErrorCode::ipc_consumer_rejected);
    f.expect_store_untouched();
}

TEST(WiredEnrolment, NoRefusedAttemptMeansNothingToEnrol) {
    Fixture f;
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error().code(), ErrorCode::trust_unknown_phone);

    // A stale refusal (phone likely gone) binds nothing either.
    ASSERT_TRUE(f.refuse(kPhone));
    f.clock.advance(aa::core::Milliseconds{aa::app::WiredEnrolment::kCandidateFreshness.count + 1});
    const auto stale = f.service.request_add_phone(f.consumer);
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().code(), ErrorCode::trust_unknown_phone);
    f.expect_store_untouched();

    // An identity with no usable material is never a candidate.
    EXPECT_FALSE(f.enrolment.note_refused(trust::WiredIdentity{}).has_value());
}

TEST(WiredEnrolment, PortOnlyPhoneIsNeverEnrolledThroughThisFlow) {
    Fixture f;
    const trust::WiredIdentity serial_less{"04e8", "6860", "", "", "", "", "1-2.3"};
    ASSERT_TRUE(f.refuse(serial_less));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error().code(), ErrorCode::trust_port_only_requires_opt_in);
    f.expect_store_untouched();
}

TEST(WiredEnrolment, CloseRefusesNewRequestsAndClosesOutstandingOnes) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    const auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.service.pairing_candidate(f.consumer, request.value()).has_value());

    f.enrolment.close();
    EXPECT_FALSE(f.enrolment.note_refused(kPhone).has_value());
    // The next request reaps the closed enrolment's outstanding request.
    const auto refused = f.service.request_add_phone(f.consumer);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(f.closed(request.value()), 1U);
    EXPECT_FALSE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
    f.expect_store_untouched();

    // After close, ForgetPhone goes to the session-side directory.
    (void)f.service.forget_phone(f.consumer, aa::core::PhoneId{7});
    ASSERT_EQ(f.after.forgotten.size(), 1U);
    EXPECT_EQ(f.after.forgotten[0], aa::core::PhoneId{7});
}

TEST(WiredEnrolment, ForgetDuringEnrolmentRevokesAndClosesTheBoundRequest) {
    Fixture f;
    ASSERT_TRUE(f.refuse(kPhone));
    auto request = f.service.request_add_phone(f.consumer);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(f.service.pairing_candidate(f.consumer, request.value()).has_value());
    ASSERT_TRUE(f.service.confirm_phone_pairing(f.consumer, request.value()).has_value());
    ASSERT_TRUE(f.service.cancel_phone_pairing(f.consumer, request.value()).has_value());
    ASSERT_TRUE(f.admitted(kPhone));
    const auto phone = f.store.phones()[0].phone_id;
    const auto forgotten = f.service.forget_phone(f.consumer, phone);
    // Revocation lands in the store even if the external (helper) leg is absent.
    (void)forgotten;
    EXPECT_FALSE(f.admitted(kPhone));
    EXPECT_TRUE(f.after.forgotten.empty());
}

} // namespace
