// SPDX-License-Identifier: GPL-3.0-or-later
// Wired identity and trust-gate tests (task 28). AOA enumeration success is
// NEVER trust: the pipeline captures the phone's pre-switch and accessory-mode
// descriptor identities and releases an endpoint only for an admitted record.
// The unknown-device case runs as a synthetic unit fixture (the second physical
// device exists but is not connected; its real-device round is pending) and
// must fail closed with transport_unauthorized_peer.
//
// Gate P1-1: connect_aoa destroys its owned factory when the pipeline fails,
// so every assertion here reads the test-owned shared LinkState - never a
// fixture object that the pipeline may have destroyed.

#include "../src/transport/usb_adapter/UsbAoaEndpoint.hpp"

#include "UsbAoaFakes.hpp"

#include <aa/core/Error.hpp>
#include <aa/session/Clock.hpp>
#include <aa/session/Lifecycle.hpp>
#include <aa/transport/UsbByteTransport.hpp>
#include <aa/trust/Identity.hpp>
#include <aa/trust/Trust.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace tr = aa::transport;
namespace ua = aa::transport::usb_adapter;
namespace fx = aa::test::usb_aoa;
using aa::ErrorCode;

tr::WiredAdmission admit_all() {
    return [](const aa::trust::WiredIdentity &) { return true; };
}

class MapTrustStore final : public aa::trust::TrustStore {
  public:
    [[nodiscard]] aa::trust::Decision
    lookup(const aa::trust::PhoneIdentity &identity) const override {
        const auto it = decisions_.find(identity.key);
        return it == decisions_.end() ? aa::trust::Decision::unknown : it->second;
    }

    aa::core::Result<void> approve_once(const aa::trust::PhoneIdentity &identity) override {
        decisions_[identity.key] = aa::trust::Decision::approved;
        return {};
    }

    aa::core::Result<void> forget(const aa::trust::PhoneIdentity &identity) override {
        decisions_.erase(identity.key);
        return {};
    }

    void set(const aa::trust::PhoneIdentity &identity, aa::trust::Decision decision) {
        decisions_[identity.key] = decision;
    }

  private:
    mutable std::map<std::string, aa::trust::Decision> decisions_{};
};

class FixedClock final : public aa::session::MonotonicClock {
  public:
    [[nodiscard]] aa::core::Nanoseconds now() const noexcept override { return now_; }

  private:
    aa::core::Nanoseconds now_{};
};

aa::trust::WiredIdentity approved_record() {
    aa::trust::WiredIdentity identity;
    identity.usb_vendor_id = "04e8";
    identity.usb_product_id = "6860";
    identity.usb_serial = "TEST-SERIAL-001";
    identity.aoa_manufacturer = "SAMSUNG";
    identity.aoa_model = "SAMSUNG_Android";
    identity.aoa_serial = "TEST-SERIAL-001";
    identity.port_context = "1-2.1";
    return identity;
}

aa::trust::PhoneIdentity phone_of(const aa::trust::WiredIdentity &identity) {
    return aa::trust::PhoneIdentity{
        aa::trust::validated_key(aa::trust::TransportIdentity{identity}).value()};
}

} // namespace

TEST(UsbAoaIdentity, ApprovedPhoneIsAdmittedAndWiredIntoLifecycle) {
    MapTrustStore store;
    ASSERT_TRUE(store.approve_once(phone_of(approved_record())).has_value());
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);

    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, tr::admit_via(store));
    ASSERT_TRUE(link.has_value());
    EXPECT_EQ(state->switches(), 1);
    EXPECT_TRUE(link.value().switch_report.started);
    EXPECT_EQ(link.value().pre_switch.vendor_id, 0x04E8);
    EXPECT_EQ(link.value().post_switch.vendor_id, 0x18D1);
    EXPECT_EQ(link.value().identity, approved_record());

    // The endpoint hands to the EXISTING framing transport (kind usb_aoa) and
    // the session Lifecycle consumes it like src/replay/Player.cpp does.
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.value().endpoint));
    ASSERT_TRUE(transport.has_value());
    EXPECT_EQ(transport.value()->kind(), tr::Kind::usb_aoa);
    FixedClock clock;
    aa::session::LifecycleConfig config{aa::session::PhaseBudget{}, aa::session::LinkKind::wired};
    aa::session::Lifecycle lifecycle(config, clock, store, transport.value().get());
    lifecycle.attach_transport(transport.value().get());
    EXPECT_EQ(lifecycle.state(), aa::session::State::disconnected);
}

TEST(UsbAoaIdentity, UnknownPhoneIsRejectedAfterSuccessfulAoaSwitch) {
    // Synthetic unapproved device: different USB ids AND different serial.
    MapTrustStore store;
    ASSERT_TRUE(store.approve_once(phone_of(approved_record())).has_value());
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);
    factory->phone.product_id = 0x9999;
    factory->phone.serial = "TEST-SERIAL-OTHER";
    factory->phone.manufacturer = "OTHER";
    factory->phone.product = "OTHER_Model";

    aa::trust::WiredIdentity attempted{};
    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, tr::admit_via(store), {}, &attempted);

    // The AOA switch still succeeded - success is never trust. Observations
    // come from the test-owned block: the pipeline destroyed its factory here.
    EXPECT_EQ(state->switches(), 1);
    EXPECT_GE(state->calls().size(), 8u);
    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code(), ErrorCode::transport_unauthorized_peer);
    // Item 2 attribution: the rejected attempt's identity+port are surfaced at
    // admission rejection so the receiver can attribute the link-error record.
    EXPECT_EQ(attempted.port_context, "1-2.1");
    EXPECT_EQ(attempted.usb_serial, "TEST-SERIAL-OTHER");
    EXPECT_EQ(attempted.usb_product_id, "9999");
    const std::string rejected_label =
        aa::trust::redacted_label(aa::trust::TransportIdentity{attempted});
    const std::string approved_label =
        aa::trust::redacted_label(aa::trust::TransportIdentity{approved_record()});
    EXPECT_NE(rejected_label, approved_label);
    EXPECT_EQ(rejected_label.find("TEST-SERIAL-OTHER"), std::string::npos);
}

TEST(UsbAoaIdentity, AdmissionSuccessLeavesAttemptedIdentityEmpty) {
    MapTrustStore store;
    ASSERT_TRUE(store.approve_once(phone_of(approved_record())).has_value());
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);

    aa::trust::WiredIdentity attempted{};
    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, tr::admit_via(store), {}, &attempted);
    ASSERT_TRUE(link.has_value());
    EXPECT_TRUE(attempted.port_context.empty());
    EXPECT_TRUE(attempted.usb_serial.empty());
}

TEST(UsbAoaIdentity, RejectedPhoneStaysUntrusted) {
    MapTrustStore store;
    store.set(phone_of(approved_record()), aa::trust::Decision::rejected);
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);

    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, tr::admit_via(store));

    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code(), ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(state->switches(), 1);
    EXPECT_EQ(state->claims(), 1);
}

TEST(UsbAoaIdentity, ForgettingThePhoneBlocksReconnect) {
    MapTrustStore store;
    const auto record = approved_record();
    ASSERT_TRUE(store.approve_once(phone_of(record)).has_value());
    {
        auto state = std::make_shared<fx::LinkState>();
        auto factory = std::make_unique<fx::FakeLinkFactory>(state);
        ASSERT_TRUE(ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                    tr::AoaAccessoryInfo{}, tr::admit_via(store))
                        .has_value());
    }
    ASSERT_TRUE(store.forget(phone_of(record)).has_value());
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);
    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, tr::admit_via(store));
    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code(), ErrorCode::transport_unauthorized_peer);
}

TEST(UsbAoaIdentity, OverlongDeviceTextFailsClosed) {
    // Untrusted external text (class untrusted_external_text): a device-supplied
    // field beyond the identity budget must fail closed - no overflow, no admit.
    MapTrustStore store;
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);
    factory->phone.serial = std::string(aa::trust::kMaxIdentityFieldBytes + 1, 'S');

    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, tr::admit_via(store));

    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code(), ErrorCode::invalid_argument);
}

TEST(UsbAoaIdentity, NonPrintableDeviceTextFailsClosed) {
    MapTrustStore store;
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);
    factory->accessory.serial = std::string("BAD\x01SERIAL");

    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, tr::admit_via(store));

    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code(), ErrorCode::invalid_argument);
}

TEST(UsbAoaIdentity, RedactedLabelNeverContainsRawSerial) {
    const auto identity = approved_record();
    const std::string label = aa::trust::redacted_label(aa::trust::TransportIdentity{identity});
    EXPECT_EQ(label.find("TEST-SERIAL-001"), std::string::npos);
    EXPECT_EQ(label.find("1-2.1"), std::string::npos);
}

TEST(UsbAoaIdentity, SelectorRejectsTraversalAndHostPaths) {
    for (const std::string &port :
         {std::string{"1-2.1/../../etc"}, std::string{"/dev/bus/usb"}, std::string{""},
          std::string{"1-"}, std::string{"abc"}, std::string{"1-2.1."}, std::string{"1-2.1;"}}) {
        auto state = std::make_shared<fx::LinkState>();
        auto factory = std::make_unique<fx::FakeLinkFactory>(state);
        auto made = ua::UsbAoaEndpoint::make(std::move(factory), tr::UsbDeviceSelector{port},
                                             tr::AoaAccessoryInfo{}, admit_all());
        ASSERT_TRUE(made.has_value());
        auto opened = made.value()->open();
        ASSERT_FALSE(opened.has_value()) << "accepted port: " << port;
        EXPECT_EQ(opened.error().code(), ErrorCode::invalid_argument) << "port: " << port;
    }
    ASSERT_TRUE(ua::validate_selector(tr::UsbDeviceSelector{"1-2.1"}).has_value());
    ASSERT_TRUE(ua::validate_selector(tr::UsbDeviceSelector{"1-2.1.2"}).has_value());
}

TEST(UsbAoaIdentity, MissingAdmissionFailsClosed) {
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);
    auto link = ua::connect_aoa(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                tr::AoaAccessoryInfo{}, nullptr);
    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code(), ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(state->switches(), 1);
}
