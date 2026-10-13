// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Probe support for aa-usb-aoa-probe (task 28, gate fix round P2-3/P2-5):
// testable units behind the target probe - the port-context grammar gate that
// runs BEFORE any sysfs access, a counted sysfs-read seam, strict JSON
// serialization of device-supplied text, and a strict bounded timeout parse.
// No external types cross this header; the grammar is the SAME gate the
// transport pipeline enforces (usb_adapter::validate_selector).

#include "../../src/transport/usb_adapter/UsbAoaDevice.hpp"

#include <charconv>
#include <chrono>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace aa::probe {

// One read-only sysfs observation of the qualified port. `serial` is captured
// for identity comparison but must never be printed raw.
struct SysfsSnapshot final {
    std::string captured_at{};
    std::string vendor{};
    std::string product{};
    std::string busnum{};
    std::string devnum{};
    std::string serial{};
    std::string manufacturer{};
    std::string device_class{};
    bool present{false};
};

// Sysfs access seam (gate P2-3): production reads /sys, tests count calls.
// read_sysfs() must never call into it before the port grammar is accepted.
class SysfsReader {
  public:
    virtual ~SysfsReader() = default;
    SysfsReader() = default;
    SysfsReader(const SysfsReader &) = delete;
    SysfsReader &operator=(const SysfsReader &) = delete;

    [[nodiscard]] virtual std::string read_file(const std::string &path) = 0;
};

// JSON string content escaping (gate P2-3): quote, backslash and control
// characters become \" \\ \u00XX so device-supplied text can never inject
// structure into the evidence document.
[[nodiscard]] inline std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    constexpr char digits[] = "0123456789abcdef";
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20U || byte == 0x7FU) {
                out += "\\u00";
                out.push_back(digits[(byte >> 4U) & 0xFU]);
                out.push_back(digits[byte & 0xFU]);
            } else {
                out.push_back(character);
            }
            break;
        }
    }
    return out;
}

// Fully quoted JSON string literal for any (possibly hostile) text.
[[nodiscard]] inline std::string json_string(std::string_view text) {
    std::string out{"\""};
    out += json_escape(text);
    out.push_back('"');
    return out;
}

// Strict timeout parse (gate P2-5): the WHOLE token must be plain decimal
// digits with no sign, whitespace or trailing characters, and the value must
// land in the inclusive 1..=300000 ms range. Never throws.
[[nodiscard]] inline bool parse_timeout_ms(std::string_view text, unsigned &out) {
    if (text.empty() || text.size() > 6) {
        return false;
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    unsigned value = 0;
    const char *first = text.data();
    const char *last = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || ptr != last) {
        return false;
    }
    if (value < 1u || value > 300000u) {
        return false;
    }
    out = value;
    return true;
}

// Port-context grammar gate (gate P2-3): the kernel USB device port naming
// `BUS-port[.port...]` (e.g. "1-2.1"), identical to the transport pipeline's
// validate_selector. Runs BEFORE any sysfs access.
[[nodiscard]] inline bool valid_port_context(const std::string &port) {
    return transport::usb_adapter::validate_selector(transport::UsbDeviceSelector{port})
        .has_value();
}

// Read the port's sysfs attributes through the seam. The grammar gate runs
// FIRST: a rejected port must produce zero reader calls (gate P2-3).
[[nodiscard]] inline SysfsSnapshot read_sysfs(const std::string &port, SysfsReader &reader) {
    SysfsSnapshot snapshot;
    if (!valid_port_context(port)) {
        return snapshot;
    }
    snapshot.captured_at = reader.read_file("now");
    snapshot.vendor = reader.read_file("idVendor");
    if (snapshot.vendor.empty()) {
        return snapshot;
    }
    snapshot.present = true;
    snapshot.product = reader.read_file("idProduct");
    snapshot.busnum = reader.read_file("busnum");
    snapshot.devnum = reader.read_file("devnum");
    snapshot.serial = reader.read_file("serial");
    snapshot.manufacturer = reader.read_file("manufacturer");
    snapshot.device_class = reader.read_file("bDeviceClass");
    return snapshot;
}

// Comma-safe JSON body for one snapshot. Device text is escaped; the serial is
// never printed raw.
[[nodiscard]] inline std::string sysfs_snapshot_json(const SysfsSnapshot &snapshot) {
    std::string out{"{\n"};
    out += "  \"present\": " + std::string(snapshot.present ? "true" : "false");
    if (snapshot.present) {
        out += ",\n  \"captured_at\": " + json_string(snapshot.captured_at);
        out += ",\n  \"idVendor\": " + json_string(snapshot.vendor);
        out += ",\n  \"idProduct\": " + json_string(snapshot.product);
        out += ",\n  \"busnum\": " + json_string(snapshot.busnum);
        out += ",\n  \"devnum\": " + json_string(snapshot.devnum);
        out += ",\n  \"manufacturer\": " + json_string(snapshot.manufacturer);
        out += ",\n  \"bDeviceClass\": " + json_string(snapshot.device_class);
        out += ",\n  \"serial\": \"<redacted>\"";
    }
    out += "\n}";
    return out;
}

// Production sysfs reader over /sys/bus/usb/devices/<port>/<attr>.
class RealSysfsReader final : public SysfsReader {
  public:
    explicit RealSysfsReader(std::string port) : port_(std::move(port)) {}

    [[nodiscard]] std::string read_file(const std::string &name) override {
        if (name == "now") {
            return now_iso();
        }
        std::ifstream in{"/sys/bus/usb/devices/" + port_ + "/" + name};
        std::string text;
        std::getline(in, text);
        return text;
    }

  private:
    [[nodiscard]] static std::string now_iso() {
        const auto wall = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(wall);
        std::tm tm{};
        ::gmtime_r(&time, &tm);
        char buffer[32] = {};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return std::string{buffer};
    }

    std::string port_;
};

} // namespace aa::probe
