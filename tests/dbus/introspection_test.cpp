// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 introspection contract equality: the runtime introspection (the
// generated embedding the D-Bus adapter serves) must match the checked schema
// XML byte-for-byte, the C++ wire-name table must match the contract methods,
// unknown major versions must be rejected, and the surface must carry no
// audio/video or other high-rate payload type (negative introspection scan).
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/Control.hpp>
#include <aa/ipc/dbus_contract.generated.hpp>

#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using aa::ipc::ControlMethod;

[[nodiscard]] std::string read_checked_schema() {
    std::ifstream stream{AA_DBUS_CONTRACT_XML};
    return std::string{std::istreambuf_iterator<char>{stream},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::set<std::string_view> wire_method_names() {
    std::set<std::string_view> names;
    for (const ControlMethod method : {ControlMethod::register_consumer,
                                       ControlMethod::unregister_consumer,
                                       ControlMethod::start_projection,
                                       ControlMethod::stop_projection,
                                       ControlMethod::request_add_phone,
                                       ControlMethod::confirm_phone_pairing,
                                       ControlMethod::cancel_phone_pairing,
                                       ControlMethod::forget_phone,
                                       ControlMethod::get_state,
                                       ControlMethod::get_capabilities,
                                       ControlMethod::set_display_viewport,
                                       ControlMethod::ping}) {
        names.insert(aa::ipc::wire_name(method));
    }
    return names;
}

TEST(Introspection, RuntimeIntrospectionMatchesCheckedSchemaFile) {
    // Given: the checked contract XML on disk and the generated embedding.
    const std::string checked = read_checked_schema();

    // When/Then: the runtime serves those exact bytes and they are non-empty.
    ASSERT_FALSE(checked.empty());
    EXPECT_EQ(aa::ipc::introspection_xml(), std::string_view{checked});
    EXPECT_EQ(aa::ipc::dbus_contract::kIntrospectionXml, std::string_view{checked});
}

TEST(Introspection, WireMethodTableMatchesGeneratedContract) {
    // Given: the C++ wire-name table and the names generated from the XML.
    const auto names = wire_method_names();

    // When/Then: membership matches in both directions (no silent drift).
    EXPECT_EQ(names.size(), aa::ipc::dbus_contract::kMethodNames.size());
    for (const auto generated : aa::ipc::dbus_contract::kMethodNames) {
        EXPECT_TRUE(names.contains(generated)) << generated;
    }
    for (const auto name : names) {
        bool found = false;
        for (const auto generated : aa::ipc::dbus_contract::kMethodNames) {
            found = found || generated == name;
        }
        EXPECT_TRUE(found) << name;
    }
}

TEST(Introspection, InterfaceConstantsMatchGeneratedContract) {
    // Given: the hand-written contract identity and the generated metadata.
    // When/Then: names, object path and SemVer all agree.
    EXPECT_EQ(aa::ipc::kContractInterface, aa::ipc::dbus_contract::kInterfaceName);
    EXPECT_EQ(aa::ipc::kContractObjectPath, aa::ipc::dbus_contract::kObjectPath);
    EXPECT_EQ(aa::ipc::contract_semver(), aa::ipc::dbus_contract::kContractSemVer);
    EXPECT_EQ(aa::ipc::contract_semver(), "1.0.0");
    EXPECT_EQ(aa::ipc::kContractMajor, 1U);
    const auto major = aa::ipc::contract_major(aa::ipc::kContractInterface);
    ASSERT_TRUE(major.has_value());
    EXPECT_EQ(*major, aa::ipc::kContractMajor);
}

TEST(Introspection, UnknownMajorVersionIsRejected) {
    // Given/When/Then: only the v1 interface name is a supported contract.
    static_assert(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver1"));
    static_assert(!aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver2"));
    static_assert(!aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver4294967297"));
    static_assert(!aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver18446744073709551617"));
    EXPECT_TRUE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver1"));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver2"));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver10"));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver01"));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver"));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver1x"));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.example.Other1"));
    EXPECT_FALSE(aa::ipc::is_supported_contract(""));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver4294967297"));
    EXPECT_FALSE(aa::ipc::is_supported_contract("org.custom.AndroidAutoReceiver18446744073709551617"));
    EXPECT_EQ(aa::ipc::contract_major("org.custom.AndroidAutoReceiver2"),
              std::optional<unsigned>{2});
    EXPECT_EQ(aa::ipc::contract_major("org.custom.Foo"), std::nullopt);
}

TEST(Introspection, OverflowingMajorDecimalsFailClosed) {
    // Given/When/Then: digit strings whose decimal value overflows unsigned
    // arithmetic never wrap into a supported major; they parse to nothing.
    static_assert(!aa::ipc::contract_major("org.custom.AndroidAutoReceiver4294967297").has_value());
    static_assert(!aa::ipc::contract_major(
                      "org.custom.AndroidAutoReceiver18446744073709551617").has_value());
    EXPECT_EQ(aa::ipc::contract_major("org.custom.AndroidAutoReceiver4294967297"),
              std::nullopt);
    EXPECT_EQ(aa::ipc::contract_major("org.custom.AndroidAutoReceiver18446744073709551617"),
              std::nullopt);
    EXPECT_EQ(aa::ipc::contract_major("org.custom.AndroidAutoReceiver4294967296"),
              std::nullopt);
    EXPECT_EQ(aa::ipc::contract_major("org.custom.AndroidAutoReceiver4294967295"),
              std::optional<unsigned>{4294967295U});
}

TEST(Introspection, SurfaceCarriesNoHighRatePayloadTypes) {
    // Given: the runtime introspection text.
    const std::string_view xml = aa::ipc::introspection_xml();
    const std::array<std::string_view, 5> allowed{"type=\"s\"", "type=\"t\"", "type=\"u\"",
                                                  "type=\"b\"", "type=\"a{ss}\""};

    // When: every declared type attribute is collected.
    std::size_t found_types = 0;
    std::size_t cursor = 0;
    for (;;) {
        const std::size_t at = xml.find("type=\"", cursor);
        if (at == std::string_view::npos) {
            break;
        }
        const std::size_t end = xml.find('"', at + 6);
        ASSERT_NE(end, std::string_view::npos);
        const std::string_view declared = xml.substr(at, end + 1 - at);
        bool ok = false;
        for (const auto permit : allowed) {
            ok = ok || declared == permit;
        }
        EXPECT_TRUE(ok) << "high-rate payload type in contract: " << declared;
        ++found_types;
        cursor = end + 1;
    }

    // Then: the surface only carries scalars and the string map; no fd,
    // byte-array, variant or struct type ever appears (the video channel is
    // the AF_UNIX contract in VideoSocket.hpp, never D-Bus), and no method
    // name advertises a media/high-rate verb.
    EXPECT_GT(found_types, 0U);
    EXPECT_EQ(xml.find("type=\"ay\""), std::string_view::npos);
    EXPECT_EQ(xml.find("type=\"h\""), std::string_view::npos);
    EXPECT_EQ(xml.find("type=\"v\""), std::string_view::npos);
    for (const auto name : aa::ipc::dbus_contract::kMethodNames) {
        EXPECT_EQ(name.find("Video"), std::string_view::npos) << name;
        EXPECT_EQ(name.find("Audio"), std::string_view::npos) << name;
        EXPECT_EQ(name.find("Frame"), std::string_view::npos) << name;
        EXPECT_EQ(name.find("Stream"), std::string_view::npos) << name;
    }
}

TEST(Introspection, SemVerMetadataIsPresentOnTheInterface) {
    // Given: the runtime introspection text.
    const std::string_view xml = aa::ipc::introspection_xml();

    // When/Then: both SemVer carriers (annotation and property) are present
    // and name the same contract version.
    const std::string_view annotation =
        "org.custom.AndroidAutoReceiver1.ContractSemVer\" value=\"1.0.0\"";
    const std::string_view property = "name=\"ContractSemVer\" type=\"s\" access=\"read\"";
    EXPECT_NE(xml.find(annotation), std::string_view::npos);
    EXPECT_NE(xml.find(property), std::string_view::npos);
}

} // namespace
