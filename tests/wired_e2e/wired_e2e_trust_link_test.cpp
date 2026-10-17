// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 30 wired link trust wiring (host-side): the USB/AOA admission callback
// is transport::admit_via(TrustStore) and the session-side phone identity is
// the exact key the store matches. An approved wired identity is admitted, an
// unknown one fails closed, and a revoke removes the admission.

#include <aa/app/WiredLink.hpp>

#include <aa/trust/Store.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace {

namespace app = aa::app;
namespace trust = aa::trust;

trust::WiredIdentity approved_identity() {
    return trust::WiredIdentity{
        "04e8", "6860", "TEST-SERIAL-001", "TEST_VENDOR", "TEST_MODEL", "TEST-SERIAL-001", "1-2.1"};
}

std::filesystem::path store_path() {
    const auto base =
        std::filesystem::temp_directory_path() / ("wired-link-trust-" + std::to_string(::getpid()));
    std::filesystem::create_directories(base);
    return base / "approved-phones.json";
}

TEST(WiredE2eTrustLink, ApprovedIdentityIsAdmittedAndUnknownFailsClosed) {
    const auto path = store_path();
    std::filesystem::remove(path);

    trust::ApprovedPhoneStore store{path};
    ASSERT_TRUE(store.load().has_value());

    const auto wired = approved_identity();
    const trust::TransportIdentity identity{wired};
    const auto admit =
        trust::allows_session(store.lookup(app::wired_phone_identity(wired).value()));
    EXPECT_FALSE(admit);

    const auto admit_unknown = aa::transport::admit_via(store);
    EXPECT_FALSE(admit_unknown(wired));

    const auto approved_id = store.approve(identity);
    ASSERT_TRUE(approved_id.has_value());

    const auto phone = app::wired_phone_identity(wired);
    ASSERT_TRUE(phone.has_value());
    EXPECT_EQ(store.lookup(phone.value()), trust::Decision::approved);
    EXPECT_TRUE(admit_unknown(wired));

    EXPECT_TRUE(store.forget_phone(approved_id.value()).has_value());
    EXPECT_FALSE(admit_unknown(wired));
    EXPECT_EQ(store.lookup(phone.value()), trust::Decision::unknown);

    std::filesystem::remove(path);
}

TEST(WiredE2eTrustLink, IdentityKeyMatchesStoreRecord) {
    const auto path = store_path();
    std::filesystem::remove(path);

    trust::ApprovedPhoneStore store{path};
    ASSERT_TRUE(store.load().has_value());
    const auto wired = approved_identity();
    ASSERT_TRUE(store.approve(trust::TransportIdentity{wired}).has_value());

    const auto phone = app::wired_phone_identity(wired);
    ASSERT_TRUE(phone.has_value());
    EXPECT_EQ(phone.value().key, trust::identity_key(trust::TransportIdentity{wired}));
    EXPECT_TRUE(trust::validated_key(trust::TransportIdentity{wired}).has_value());

    trust::WiredIdentity other = wired;
    other.usb_serial = "TEST-SERIAL-002";
    const auto other_phone = app::wired_phone_identity(other);
    ASSERT_TRUE(other_phone.has_value());
    EXPECT_NE(other_phone.value().key, phone.value().key);
    EXPECT_EQ(store.lookup(other_phone.value()), trust::Decision::unknown);

    std::filesystem::remove(path);
}

} // namespace
