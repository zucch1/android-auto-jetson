// SPDX-License-Identifier: GPL-3.0-or-later
// Wired USB/AOA target probe (task 28). Runs the production connection
// pipeline against the qualified phone on the Jetson and reports the two
// independent acceptance observables:
//   1. phone re-enumeration 04e8:6860 -> 18d1:2d00 on the same port path
//      (sysfs snapshots before and after, with timestamps), and
//   2. framed transport ready: a framed message produced by
//      aa::transport::UsbByteTransport actually traverses the bulk endpoints
//      (send on OUT 0x01 and a framed reply read from IN 0x81).
// AOA enumeration success is never trust: the captured wired identity must
// pass admission (a recorded identity file, or --admit observed for wiring
// runs). Exit 0 requires BOTH observables; a probe exit 0 without them is not
// a pass (misleading_success_output rule). Serials never print: identities are
// reported through trust::redacted_label only.

#include <aa/core/Cancellation.hpp>
#include <aa/core/Error.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/UsbAoa.hpp>
#include <aa/transport/UsbByteTransport.hpp>
#include <aa/trust/Identity.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace tr = aa::transport;

inline constexpr std::uint16_t kGoogleVendor = 0x18D1;
inline constexpr std::uint16_t kAccessoryProduct = 0x2D00;
inline constexpr std::uint16_t kAccessoryProductAdb = 0x2D01;

std::string hex4(std::uint16_t value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase;
    out.width(4);
    out.fill('0');
    out << value;
    return out.str();
}

std::string hex_byte(std::byte value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase;
    out.width(2);
    out.fill('0');
    out << static_cast<unsigned>(value);
    return out.str();
}

std::string now_iso() {
    const auto wall = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(wall);
    std::tm tm{};
    ::gmtime_r(&time, &tm);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string read_text(const std::string &path) {
    std::ifstream in{path};
    std::string text;
    std::getline(in, text);
    return text;
}

// Read-only sysfs snapshot of the qualified port (device evidence for the
// re-enumeration observable). `serial` is captured but never printed raw.
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

SysfsSnapshot read_sysfs(const std::string &port) {
    SysfsSnapshot snapshot;
    snapshot.captured_at = now_iso();
    const std::string base = "/sys/bus/usb/devices/" + port;
    snapshot.vendor = read_text(base + "/idVendor");
    if (snapshot.vendor.empty()) {
        return snapshot;
    }
    snapshot.present = true;
    snapshot.product = read_text(base + "/idProduct");
    snapshot.busnum = read_text(base + "/busnum");
    snapshot.devnum = read_text(base + "/devnum");
    snapshot.serial = read_text(base + "/serial");
    snapshot.manufacturer = read_text(base + "/manufacturer");
    snapshot.device_class = read_text(base + "/bDeviceClass");
    return snapshot;
}

void print_sysfs(const SysfsSnapshot &snapshot) {
    std::cout << "  \"present\": " << (snapshot.present ? "true" : "false") << ",\n";
    if (!snapshot.present) {
        return;
    }
    std::cout << "  \"captured_at\": \"" << snapshot.captured_at << "\",\n";
    std::cout << "  \"idVendor\": \"" << snapshot.vendor << "\",\n";
    std::cout << "  \"idProduct\": \"" << snapshot.product << "\",\n";
    std::cout << "  \"busnum\": \"" << snapshot.busnum << "\",\n";
    std::cout << "  \"devnum\": \"" << snapshot.devnum << "\",\n";
    std::cout << "  \"manufacturer\": \"" << snapshot.manufacturer << "\",\n";
    std::cout << "  \"bDeviceClass\": \"" << snapshot.device_class << "\",\n";
    std::cout << "  \"serial\": \"<redacted>\"\n";
}

bool load_identity_file(const std::string &path, aa::trust::WiredIdentity &identity,
                        std::string &error) {
    std::ifstream in{path};
    if (!in) {
        error = "identity-file-unreadable";
        return false;
    }
    std::map<std::string, std::string *> fields{
        {"usb_vendor_id", &identity.usb_vendor_id},
        {"usb_product_id", &identity.usb_product_id},
        {"usb_serial", &identity.usb_serial},
        {"aoa_manufacturer", &identity.aoa_manufacturer},
        {"aoa_model", &identity.aoa_model},
        {"aoa_serial", &identity.aoa_serial},
        {"port_context", &identity.port_context},
    };
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t split = line.find('=');
        if (split == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, split);
        const auto found = fields.find(key);
        if (found != fields.end()) {
            *found->second = line.substr(split + 1);
        }
    }
    if (identity.port_context.empty()) {
        error = "identity-file-incomplete";
        return false;
    }
    return true;
}

tr::WiredAdmission admit_observed() {
    return [](const aa::trust::WiredIdentity &) { return true; };
}

tr::WiredAdmission admit_record(const aa::trust::WiredIdentity &recorded) {
    return [recorded](const aa::trust::WiredIdentity &observed) {
        return aa::trust::identity_key(aa::trust::TransportIdentity{observed}) ==
               aa::trust::identity_key(aa::trust::TransportIdentity{recorded});
    };
}

struct Options final {
    std::string port{"1-2.1"};
    std::uint16_t vendor{0x04E8};
    std::uint16_t product{0x6860};
    std::string identity_file{};
    bool admit_observed_only{false};
    bool recover_reset{true};
    unsigned read_timeout_ms{5000};
};

bool parse_hex16(const std::string &text, std::uint16_t &out) {
    if (text.size() != 4) {
        return false;
    }
    unsigned value = 0;
    std::istringstream in{text};
    in >> std::hex >> value;
    if (!in || value > 0xFFFFu) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

// AAP VersionRequest control payload: 2-byte big-endian ControlMessageId 1
// followed by two big-endian uint16 version fields (include/aa/protocol/
// Messages.hpp wire shape; the protocol adapter is host-only so the probe
// restates this fixed-width body).
std::vector<std::byte> version_request_payload(std::uint16_t major, std::uint16_t minor) {
    return {
        std::byte{0x00},
        std::byte{0x01},
        static_cast<std::byte>((major >> 8U) & 0xFFU),
        static_cast<std::byte>(major & 0xFFU),
        static_cast<std::byte>((minor >> 8U) & 0xFFU),
        static_cast<std::byte>(minor & 0xFFU),
    };
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return std::string{argv[++i]};
        };
        if (arg == "--port") {
            options.port = next("--port");
        } else if (arg == "--pre-vid") {
            if (!parse_hex16(next("--pre-vid"), options.vendor)) {
                std::cerr << "invalid --pre-vid\n";
                return 2;
            }
        } else if (arg == "--pre-pid") {
            if (!parse_hex16(next("--pre-pid"), options.product)) {
                std::cerr << "invalid --pre-pid\n";
                return 2;
            }
        } else if (arg == "--identity-file") {
            options.identity_file = next("--identity-file");
        } else if (arg == "--admit") {
            const std::string mode = next("--admit");
            if (mode == "observed") {
                options.admit_observed_only = true;
            } else if (mode != "file") {
                std::cerr << "invalid --admit (file|observed)\n";
                return 2;
            }
        } else if (arg == "--read-timeout-ms") {
            options.read_timeout_ms = static_cast<unsigned>(std::stoul(next("--read-timeout-ms")));
        } else if (arg == "--no-recover-reset") {
            options.recover_reset = false;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    tr::UsbDeviceSelector selector;
    selector.port_context = options.port;
    selector.vendor_id = options.vendor;
    selector.product_id = options.product;

    aa::trust::WiredIdentity recorded{};
    tr::WiredAdmission admit = admit_observed();
    std::string admit_mode = "observed";
    if (!options.admit_observed_only) {
        if (options.identity_file.empty()) {
            std::cerr << "--identity-file is required unless --admit observed\n";
            return 2;
        }
        std::string error;
        if (!load_identity_file(options.identity_file, recorded, error)) {
            std::cerr << error << "\n";
            return 2;
        }
        admit = admit_record(recorded);
        admit_mode = "file";
    }

    const SysfsSnapshot sysfs_before = read_sysfs(options.port);
    std::cout << "{\n";
    std::cout << "\"probe\": \"aa-usb-aoa-probe/task-28/1\",\n";
    std::cout << "\"port\": \"" << options.port << "\",\n";
    std::cout << "\"admit_mode\": \"" << admit_mode << "\",\n";
    std::cout << "\"sysfs_before\": {\n";
    print_sysfs(sysfs_before);
    std::cout << "},\n";

    // Recovery: a device left in accessory mode by an earlier run gets one
    // targeted software reset so the full switch can be observed again.
    // Recovery observes the PORT unfiltered: the phone-id selector would hide
    // a device already left in accessory mode by an earlier run.
    tr::UsbDeviceSelector port_only;
    port_only.port_context = options.port;
    auto observed = tr::observe_usb_device(port_only);
    bool recovery_reset = false;
    if (observed.has_value() && observed.value().vendor_id == kGoogleVendor &&
        (observed.value().product_id == kAccessoryProduct ||
         observed.value().product_id == kAccessoryProductAdb)) {
        if (!options.recover_reset) {
            std::cout << "\"recovery\": \"device-already-in-accessory-mode\",\n";
            std::cout << "\"result\": \"not-run\"\n}\n";
            return 7;
        }
        recovery_reset = true;
        auto reset = tr::reset_port_device(port_only);
        std::string reset_status = "ok";
        if (!reset) {
            // A software reset re-enumerates the device, so libusb may report
            // the dying handle as an error even when the reset worked: keep
            // waiting for phone mode either way and record what was seen.
            reset_status = aa::to_string(reset.error().code());
        }
        std::cout << "\"recovery_reset_status\": \"" << reset_status << "\",\n";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            auto again = tr::observe_usb_device(selector);
            if (again.has_value() && again.value().vendor_id != kGoogleVendor) {
                break;
            }
        }
    }
    std::cout << "\"recovery\": \"" << (recovery_reset ? "reset-performed" : "not-needed")
              << "\",\n";

    const auto connect_start = std::chrono::steady_clock::now();
    auto link = tr::connect_usb_aoa(selector, tr::AoaAccessoryInfo{}, admit);
    const auto connect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connect_start);
    if (!link) {
        std::cout << "\"connect_error\": \"" << aa::to_string(link.error().code()) << "\",\n";
        std::cout << "\"connect_elapsed_ms\": " << connect_elapsed.count() << ",\n";
        std::cout << "\"result\": \"connect-failed\"\n}\n";
        return 2;
    }

    const tr::UsbAoaLink &record = link.value();
    std::cout << "\"connect_elapsed_ms\": " << connect_elapsed.count() << ",\n";
    std::cout << "\"switch\": {\n";
    std::cout << "  \"protocol_version\": " << record.switch_report.protocol_version << ",\n";
    std::cout << "  \"started\": " << (record.switch_report.started ? "true" : "false") << ",\n";
    std::cout << "  \"requests\": " << record.switch_report.requests << ",\n";
    std::cout << "  \"failed_at\": \"" << record.switch_report.failed_at << "\",\n";
    std::cout << "  \"steps\": [";
    for (std::size_t i = 0; i < record.switch_report.steps.size(); ++i) {
        const auto &step = record.switch_report.steps[i];
        std::cout << (i == 0 ? "" : ", ") << "{\"index\": " << step.index
                  << ", \"expected\": " << step.expected_bytes
                  << ", \"observed\": " << step.observed_bytes << "}";
    }
    std::cout << "]\n},\n";
    std::cout << "\"pre_switch\": {\"idVendor\": \"" << hex4(record.pre_switch.vendor_id)
              << "\", \"idProduct\": \"" << hex4(record.pre_switch.product_id)
              << "\", \"bus\": " << record.pre_switch.bus
              << ", \"address\": " << record.pre_switch.address
              << ", \"serial\": \"<redacted>\"},\n";
    std::cout << "\"post_switch\": {\"idVendor\": \"" << hex4(record.post_switch.vendor_id)
              << "\", \"idProduct\": \"" << hex4(record.post_switch.product_id)
              << "\", \"bus\": " << record.post_switch.bus
              << ", \"address\": " << record.post_switch.address
              << ", \"serial\": \"<redacted>\"},\n";
    std::cout << "\"identity_redacted\": \""
              << aa::trust::redacted_label(aa::trust::TransportIdentity{record.identity})
              << "\",\n";

    const bool reenumeration = record.pre_switch.vendor_id == options.vendor &&
                               record.post_switch.vendor_id == kGoogleVendor &&
                               (record.post_switch.product_id == kAccessoryProduct ||
                                record.post_switch.product_id == kAccessoryProductAdb);
    std::cout << "\"re_enumeration\": \"" << (reenumeration ? "observed" : "not-observed")
              << "\",\n";

    auto transport = tr::UsbByteTransport::over_endpoint(std::move(link.value().endpoint));
    if (!transport) {
        std::cout << "\"transport_error\": \"over-endpoint-failed\",\n";
        std::cout << "\"result\": \"transport-failed\"\n}\n";
        return 3;
    }
    aa::core::CancellationSource source;
    auto opened = transport.value()->open(source.get_token());
    if (!opened) {
        std::cout << "\"transport_error\": \"" << aa::to_string(opened.error().code()) << "\",\n";
        std::cout << "\"result\": \"transport-failed\"\n}\n";
        return 3;
    }
    std::cout << "\"transport_kind\": \""
              << (transport.value()->kind() == tr::Kind::usb_aoa ? "usb_aoa" : "other") << "\",\n";

    // AASDK's wire shape for the control-channel VersionRequest (the pinned
    // reference, ControlServiceChannel::sendVersionRequest): SPECIFIC message
    // kind, big-endian message id 1, then AASDK_MAJOR.6 as big-endian uint16s.
    const tr::FrameHeader header{0x00, tr::FrameType::bulk, tr::MessageKind::specific,
                                 tr::Encryption::plain};
    auto frame = tr::encode_frame(header, version_request_payload(1, 6), 0);
    bool send_ok = false;
    if (frame) {
        send_ok = transport.value()->send(frame.value()).has_value();
    }
    std::cout << "\"framed_send\": \"" << (send_ok ? "ok" : "failed") << "\",\n";
    std::cout << "\"framed_send_bytes\": " << (frame ? frame.value().size() : 0) << ",\n";

    std::thread watchdog([&source, &options] {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.read_timeout_ms));
        source.request_stop();
    });
    auto received = transport.value()->receive();
    watchdog.join();
    const bool receive_ok = received.has_value();
    std::cout << "\"framed_receive\": \"" << (receive_ok ? "ok" : "no-reply") << "\",\n";
    if (receive_ok) {
        std::cout << "\"framed_reply_bytes\": " << received.value().size() << ",\n";
        std::cout << "\"framed_reply_prefix_hex\": \"";
        for (std::size_t i = 0; i < received.value().size() && i < 12; ++i) {
            std::cout << hex_byte(received.value()[i]);
        }
        std::cout << "\",\n";
    } else {
        std::cout << "\"framed_receive_error\": \"" << aa::to_string(received.error().code())
                  << "\",\n";
    }

    transport.value()->close();
    const SysfsSnapshot sysfs_after = read_sysfs(options.port);
    std::cout << "\"sysfs_after\": {\n";
    print_sysfs(sysfs_after);
    std::cout << "},\n";

    const bool framed_round_trip = send_ok && receive_ok;
    std::cout << "\"framed_round_trip\": \"" << (framed_round_trip ? "observed" : "not-observed")
              << "\",\n";
    if (reenumeration && framed_round_trip) {
        std::cout << "\"result\": \"framed_transport_ready\"\n}\n";
        return 0;
    }
    std::cout << "\"result\": \"partial\"\n}\n";
    return framed_round_trip ? 6 : 5;
}
