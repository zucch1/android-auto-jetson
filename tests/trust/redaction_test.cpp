// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 redaction (task-19 rules): raw Bluetooth MACs, phone identifiers and
// USB serials must never appear in labels or store summaries handed to
// logs/diagnostics — only per-process pseudonyms do.
#include <aa/trust/Identity.hpp>
#include <aa/trust/Store.hpp>

#include "fakes.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(TrustRedaction, RedactedLabelOmitsRawIdentifiers) {
    const trust::TransportIdentity wired{test::kWired};
    const std::string label = trust::redacted_label(wired);

    EXPECT_FALSE(label.empty());
    EXPECT_FALSE(contains(label, "SERIAL-42"));
    EXPECT_FALSE(contains(label, "AOA-42"));
    EXPECT_FALSE(contains(label, "1-2.3"));
    // The label is exactly "id-" plus a salted 64-bit FNV-1a digest in hex, so
    // no raw field can appear outside the digest. Hex-only identifiers such as
    // the vendor id "18d1" are not checked by substring: a per-process salted
    // digest contains any given 4-hex-digit string by chance (~13/65536 runs).
    ASSERT_EQ(label.size(), 3u + 16u) << label;
    EXPECT_EQ(label.substr(0, 3), "id-");
    EXPECT_EQ(label.find_first_not_of("0123456789abcdef", 3), std::string::npos) << label;
}

TEST(TrustRedaction, RedactedLabelOmitsBluetoothAddress) {
    const trust::TransportIdentity wireless{test::kWireless};
    const std::string label = trust::redacted_label(wireless);

    EXPECT_FALSE(contains(label, "AA:BB:CC:DD:EE:FF"));
    EXPECT_FALSE(contains(label, "aa:bb:cc:dd:ee:ff"));
    EXPECT_TRUE(contains(label, "id-"));
}

TEST(TrustRedaction, RedactedLabelIsStableWithinProcess) {
    const trust::TransportIdentity wired{test::kWired};
    EXPECT_EQ(trust::redacted_label(wired), trust::redacted_label(wired));
}

TEST(TrustRedaction, DistinctIdentitiesGetDistinctPseudonyms) {
    const trust::TransportIdentity wired{test::kWired};
    const trust::TransportIdentity wireless{test::kWireless};
    EXPECT_NE(trust::redacted_label(wired), trust::redacted_label(wireless));
}

TEST(TrustRedaction, StoreSummaryCarriesOnlyPseudonyms) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWired}).has_value());
    ASSERT_TRUE(store.approve(trust::TransportIdentity{test::kWireless}).has_value());

    const std::string summary = store.redacted_summary();
    EXPECT_FALSE(contains(summary, "SERIAL-42"));
    EXPECT_FALSE(contains(summary, "AOA-42"));
    EXPECT_FALSE(contains(summary, "AA:BB:CC:DD:EE:FF"));
    EXPECT_FALSE(contains(summary, "1-2.3"));
    EXPECT_TRUE(contains(summary, "phone-1"));
    EXPECT_TRUE(contains(summary, "phone-2"));
}

} // namespace
