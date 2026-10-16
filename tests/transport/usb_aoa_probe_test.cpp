// SPDX-License-Identifier: GPL-3.0-or-later
// Probe qualification-safety tests (task 28 gate fix round P2-3/P2-5): the
// port-context grammar gate runs before any sysfs access, device-supplied text
// is JSON-escaped (no structure injection), the absent-device document is
// comma-safe JSON, and the timeout parse is strict (no exceptions, no SIGABRT,
// bounded 1..300000 ms).

#include "../../tools/usb-aoa-probe/probe_support.hpp"
#include "../src/transport/usb_adapter/UsbAoaDevice.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace probe = aa::probe;
namespace ua = aa::transport::usb_adapter;

class CountingReader final : public probe::SysfsReader {
  public:
    std::vector<std::string> paths{};

    std::string read_file(const std::string &path) override {
        paths.push_back(path);
        return script.empty() ? std::string{} : script;
    }

    std::string script{};
};

TEST(UsbAoaProbe, PortGrammarAcceptsOnlyKernelPortPaths) {
    for (const std::string &port : {std::string{"1-2.1"}, std::string{"1-2"},
                                    std::string{"1-2.1.2"}, std::string{"2-3.4.5.6"}}) {
        EXPECT_TRUE(probe::valid_port_context(port)) << "rejected: " << port;
    }
    for (const std::string &port :
         {std::string{}, std::string{"1-"}, std::string{"-2.1"}, std::string{"1-2."},
          std::string{"1-2.1;"}, std::string{"1-2.1/../../etc"}, std::string{"/dev/bus/usb"},
          std::string{"1 2.1"}, std::string{"1-2.1\n"}, std::string{"1-2.1\""},
          std::string{"1-2.1\"; \"framed_round_trip\": \"observed"}, std::string{"999'"},
          std::string{"1-2.1.0x"}, std::string(65, '9')}) {
        EXPECT_FALSE(probe::valid_port_context(port)) << "accepted hostile: " << port.size();
    }
}

TEST(UsbAoaProbe, PortGrammarAgreesWithTransportPipelineGate) {
    for (const std::string &port :
         {std::string{"1-2.1"}, std::string{"1-2"}, std::string{""}, std::string{"1-2.1/../../etc"},
          std::string{"1-2.1;"}, std::string{"1-2.1.\""}}) {
        const bool seam = probe::valid_port_context(port);
        const bool pipeline =
            ua::validate_selector(aa::transport::UsbDeviceSelector{port}).has_value();
        EXPECT_EQ(seam, pipeline) << "divergent grammar for port of size " << port.size();
    }
}

TEST(UsbAoaProbe, HostilePortIsRejectedBeforeAnyFsSeamCall) {
    for (const std::string &port :
         {std::string{"1-2.1/../../etc"}, std::string{"1-2.1\"; \"framed_round_trip\": \"observed"},
          std::string{}, std::string{"1-2.1\n"}}) {
        CountingReader reader;
        const probe::SysfsSnapshot snapshot = probe::read_sysfs(port, reader);
        EXPECT_FALSE(snapshot.present);
        EXPECT_TRUE(reader.paths.empty()) << "fs seam called for hostile port";
    }
}

TEST(UsbAoaProbe, ValidPortReachesFsSeamAndToleratesAbsentDevice) {
    CountingReader reader;
    const probe::SysfsSnapshot snapshot = probe::read_sysfs("1-2.1", reader);
    EXPECT_FALSE(snapshot.present);
    ASSERT_EQ(reader.paths.size(), 2u);
    EXPECT_EQ(reader.paths[0], "now");
    EXPECT_EQ(reader.paths[1], "idVendor");
}

TEST(UsbAoaProbe, AbsentDeviceSnapshotIsCommaSafeJson) {
    CountingReader reader;
    const probe::SysfsSnapshot snapshot = probe::read_sysfs("1-2.1", reader);
    const std::string json = probe::sysfs_snapshot_json(snapshot);
    EXPECT_EQ(json, std::string{"{\n  \"present\": false\n}"});
    EXPECT_EQ(json.find(",\n}"), std::string::npos);
    EXPECT_EQ(json.find(", }"), std::string::npos);
}

TEST(UsbAoaProbe, DeviceTextIsJsonEscapedNeverInjected) {
    probe::SysfsSnapshot snapshot;
    snapshot.present = true;
    snapshot.captured_at = "2026-10-13T00:00:00Z";
    snapshot.vendor = "04e8";
    snapshot.product = "6860";
    snapshot.busnum = "1";
    snapshot.devnum = "2";
    snapshot.serial = "SECRET-SERIAL";
    snapshot.manufacturer = "EVIL\\\", \"framed_round_trip\": \"observed\n\t\x01";
    snapshot.device_class = "00";

    const std::string json = probe::sysfs_snapshot_json(snapshot);

    // The hostile text lands ONLY in the escaped value position: its quote
    // cannot close the JSON string, its newline cannot break line structure,
    // and the key it attempts to inject never appears as a structural key.
    const std::string escaped_value = probe::json_string(snapshot.manufacturer);
    EXPECT_NE(json.find("\"manufacturer\": " + escaped_value), std::string::npos);
    EXPECT_EQ(json.find("observed\n"), std::string::npos);
    EXPECT_EQ(json.find("\n  \"framed_round_trip\""), std::string::npos);
    EXPECT_NE(json.find("\\n"), std::string::npos);
    EXPECT_NE(json.find("\\u0001"), std::string::npos);
    // Serial is never printed raw.
    EXPECT_EQ(json.find("SECRET-SERIAL"), std::string::npos);
    EXPECT_NE(json.find("\"<redacted>\""), std::string::npos);
}

TEST(UsbAoaProbe, JsonStringQuotesAndEscapesControlCharacters) {
    EXPECT_EQ(probe::json_string("plain"), std::string{"\"plain\""});
    EXPECT_EQ(probe::json_string("a\"b"), std::string{"\"a\\\"b\""});
    EXPECT_EQ(probe::json_string("a\\b"), std::string{"\"a\\\\b\""});
    EXPECT_EQ(probe::json_string("a\nb"), std::string{"\"a\\nb\""});
    EXPECT_EQ(probe::json_string(std::string{"a\x01\x1F"}), std::string{"\"a\\u0001\\u001f\""});
    EXPECT_EQ(probe::json_string(""), std::string{"\"\""});
}

// Exact byte strings for hostile-input cases (hex escapes in literals would
// bleed into adjacent digits; bytes are stated numerically instead).
std::string raw(std::initializer_list<int> byte_values) {
    std::string out;
    for (const int value : byte_values) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
    }
    return out;
}

TEST(UsbAoaProbe, JsonEscapeValidUtf8RoundTripsVerbatim) {
    // Fix round P2-1: a valid multibyte character must decode back to the
    // same text, so valid RFC 3629 sequences pass through UNCHANGED - never
    // naive per-byte \u00XX, which would mojibake them.
    for (const std::string &text :
         {std::string{"ascii"}, raw({0xC3, 0xA9}), raw({0xE2, 0x82, 0xAC}),
          raw({0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC}), raw({0xF0, 0x9F, 0x98, 0x80}),
          raw({0xC2, 0x80}), raw({0xDF, 0xBF}), raw({0xE0, 0xA0, 0x80}), raw({0xEF, 0xBF, 0xBF}),
          raw({0xF0, 0x90, 0x80, 0x80}), raw({0xF4, 0x8F, 0xBF, 0xBF}),
          raw({'a', 0xC3, 0xA9, 'b'})}) {
        EXPECT_EQ(probe::json_escape(text), text) << "valid UTF-8 must round-trip verbatim";
    }
    // JSON specials adjacent to multibyte text are still escaped - only the
    // specials change, the character does not.
    EXPECT_EQ(probe::json_string(raw({'"', 0xC3, 0xA9, '\\'})),
              std::string{"\"\\\""} + raw({0xC3, 0xA9}) + "\\\\\"");
}

TEST(UsbAoaProbe, JsonEscapeInvalidBytesBecomeUfffdNeverRaw) {
    // Fix round P2-1: bytes outside any valid UTF-8 sequence are replaced with
    // U+FFFD (one \ufffd per offending byte) so the JSON body stays valid
    // UTF-8 for arbitrary hostile input.
    const std::string one = "\\ufffd";
    EXPECT_EQ(probe::json_escape(raw({0xFF})), one);
    EXPECT_EQ(probe::json_escape(raw({0xFE})), one);
    EXPECT_EQ(probe::json_escape(raw({0x80})), one);
    EXPECT_EQ(probe::json_escape(raw({0xBF})), one);
    EXPECT_EQ(probe::json_escape(raw({0xC0, 0xAF})), one + one);             // overlong '/'
    EXPECT_EQ(probe::json_escape(raw({0xC1, 0xBF})), one + one);             // overlong 0x7F
    EXPECT_EQ(probe::json_escape(raw({0xE0, 0x80, 0xAF})), one + one + one); // overlong 3-byte
    EXPECT_EQ(probe::json_escape(raw({0xE0, 0x9F, 0xBF})), one + one + one); // overlong U+07FF
    EXPECT_EQ(probe::json_escape(raw({0xF0, 0x80, 0x80, 0xAF})), one + one + one + one);
    EXPECT_EQ(probe::json_escape(raw({0xE2, 0x82})), one + one);             // truncated U+20AC
    EXPECT_EQ(probe::json_escape(raw({0xF0, 0x9F, 0x98})), one + one + one); // truncated U+1F600
    EXPECT_EQ(probe::json_escape(raw({0xED, 0xA0, 0x80})), one + one + one); // surrogate half
    EXPECT_EQ(probe::json_escape(raw({0xED, 0xBF, 0xBF})), one + one + one); // surrogate half
    EXPECT_EQ(probe::json_escape(raw({0xF4, 0x90, 0x80, 0x80})),
              one + one + one + one); // > U+10FFFF
    EXPECT_EQ(probe::json_escape(raw({0xF5, 0x80, 0x80, 0x80})), one + one + one + one);
    EXPECT_EQ(probe::json_escape(raw({0xC3})), one); // truncated 2-byte
    // Mixed hostile text: invalid bytes are replaced, valid characters kept.
    EXPECT_EQ(probe::json_escape(raw({'a', 0xFF, 0xC3, 0xA9, 0x80, 'b'})),
              std::string{"a"} + one + raw({0xC3, 0xA9}) + one + "b");
}

TEST(UsbAoaProbe, JsonEscapeNulAndDelAreEscaped) {
    // NUL cannot ride argv (execve is NUL-terminated) but can arrive in
    // device-supplied text; it must never terminate the JSON body early.
    EXPECT_EQ(probe::json_escape(raw({0x00})), std::string{"\\u0000"});
    EXPECT_EQ(probe::json_string(raw({'a', 0x00, 'b'})), std::string{"\"a\\u0000b\""});
    EXPECT_EQ(probe::json_escape(raw({0x7F})), std::string{"\\u007f"});
    EXPECT_EQ(probe::json_escape(raw({0x00, 0x1F, 0x7F})), std::string{"\\u0000\\u001f\\u007f"});
}

TEST(UsbAoaProbe, TimeoutParseAcceptsOnlyPlainBoundedDecimal) {
    unsigned value = 0;
    EXPECT_TRUE(probe::parse_timeout_ms("1", value));
    EXPECT_EQ(value, 1u);
    EXPECT_TRUE(probe::parse_timeout_ms("5000", value));
    EXPECT_EQ(value, 5000u);
    EXPECT_TRUE(probe::parse_timeout_ms("300000", value));
    EXPECT_EQ(value, 300000u);

    for (const std::string &bad :
         {std::string{"not-a-number"}, std::string{"12abc"}, std::string{"-5"}, std::string{"0"},
          std::string{"99999999999999999999"}, std::string{"300001"}, std::string{""},
          std::string{" 5"}, std::string{"5 "}, std::string{"+5"}, std::string{"0x10"},
          std::string{"5.0"}, std::string{"٥"}, std::string{std::string(20, '9')}}) {
        value = 4242;
        EXPECT_FALSE(probe::parse_timeout_ms(bad, value)) << "accepted: size " << bad.size();
        EXPECT_EQ(value, 4242u) << "out param mutated on failure";
    }
}

} // namespace
