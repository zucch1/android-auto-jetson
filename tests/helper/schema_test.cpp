// SPDX-License-Identifier: GPL-3.0-or-later
#include <gio/gio.h>
#include <gtest/gtest.h>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace {
std::string signature(GDBusArgInfo** args) {
    std::string result;
    for (std::size_t index = 0; args && args[index]; ++index) result += args[index]->signature;
    return result;
}
TEST(HelperSchema, ComposedContractPreservesNarrowApAndPeerMethods) {
    std::ifstream file{std::string{AA_HELPER_SOURCE} + "/docs/dbus/org.custom.AndroidAutoSystemHelper1.xml"};
    ASSERT_TRUE(file);
    const std::string xml{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    std::unique_ptr<GDBusNodeInfo, decltype(&g_dbus_node_info_unref)> node{
        g_dbus_node_info_new_for_xml(xml.c_str(), nullptr), g_dbus_node_info_unref};
    ASSERT_TRUE(node); ASSERT_NE(node->interfaces[0], nullptr);
    EXPECT_STREQ(node->interfaces[0]->name, "org.custom.AndroidAutoSystemHelper1");
    EXPECT_EQ(node->interfaces[1], nullptr);
    using Signatures = std::pair<std::string, std::string>;
    std::map<std::string, Signatures> actual;
    for (auto** method = node->interfaces[0]->methods; *method; ++method)
        EXPECT_TRUE(actual.emplace((*method)->name, Signatures{signature((*method)->in_args), signature((*method)->out_args)}).second);
    const std::map<std::string, Signatures> expected{
        {"Issue", {"ttuu", "ut"}}, {"Execute", {"uut", "ut"}}, {"Invalidate", {"tt", "ut"}},
        {"RevokeAP1", {"t", "ut"}}, {"PollPeerV1", {"", "utss"}}, {"BindPeerV1", {"ttt", "ut"}},
        {"ReconnectPeerV1", {"t", "ut"}}, {"RevokePeerV1", {"ss", "ut"}}};
    EXPECT_EQ(actual, expected);
}
}
