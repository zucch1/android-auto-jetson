// SPDX-License-Identifier: GPL-3.0-or-later
// Wired USB/AOA target probe (task 28). Runs the production connection
// pipeline against the qualified phone on the Jetson and reports the two
// independent acceptance observables:
//   1. phone re-enumeration 04e8:6860 -> 18d1:2d00/2d01 on the same port path
//      (sysfs snapshots before and after, with timestamps), and
//   2. framed transport ready: a framed message produced by
//      aa::transport::UsbByteTransport actually traverses the bulk endpoints
//      (send on OUT 0x01 and a framed reply read from IN 0x81).
// AOA enumeration success is never trust: the captured wired identity MUST
// match the admission file (--identity-file, the only admission mode). Exit 0
// requires BOTH observables; a probe exit 0 without them is not a pass.
// Serials never print: identities are reported through trust::redacted_label
// only. Every device-supplied string is JSON-escaped and the port grammar is
// validated before any sysfs access (gate P2-3).
//
// Exit codes: 0 framed transport ready (both observables); 2 usage error
// (bad arguments, invalid port grammar, missing/unreadable identity file,
// parse failures); 3 transport setup failed (connect/AOA switch/identity
// admission failures); 5 no framed reply; 6 framed
// round-trip without re-enumeration; 7 recovery could not restore phone mode.

#include "probe_support.hpp"

#include <aa/core/Cancellation.hpp>
#include <aa/core/Error.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/UsbAoa.hpp>
#include <aa/transport/UsbByteTransport.hpp>
#include <aa/trust/Identity.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace probe = aa::probe;
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

void print_usage(std::ostream &out) {
    out << "usage: aa-usb-aoa-probe --port PORT --pre-vid VVVV --pre-pid PPPP "
           "--identity-file PATH [--read-timeout-ms MS] [--no-recover-reset]\n"
           "       aa-usb-aoa-probe --help\n"
           "exit codes: 0 framed transport ready (re-enumeration AND framed "
           "round-trip); 2 usage error; 3 transport setup failed; 5 no framed "
           "reply; 6 round-trip without re-enumeration; 7 recovery failed\n";
}

[[noreturn]] void usage_error(const std::string &message) {
    // stderr gets the escaped form (terminal-safe); the JSON document escapes
    // the raw message exactly once so no external text can inject structure.
    std::cerr << "usage error: " << probe::json_escape(message) << "\n";
    std::cout << "{\"probe\": \"aa-usb-aoa-probe/task-28/2\", \"error\": "
              << probe::json_string(message) << ", \"result\": \"usage-error\"}\n";
    std::exit(2);
}

bool parse_hex16(const std::string &text, std::uint16_t &out) {
    if (text.size() != 4) {
        return false;
    }
    unsigned value = 0;
    std::istringstream in{text};
    in >> std::hex >> value;
    if (!in || in.peek() != std::char_traits<char>::eof() || value > 0xFFFFu) {
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
    return {std::byte{0x00},
            std::byte{0x01},
            static_cast<std::byte>((major >> 8U) & 0xFFU),
            static_cast<std::byte>(major & 0xFFU),
            static_cast<std::byte>((minor >> 8U) & 0xFFU),
            static_cast<std::byte>(minor & 0xFFU)};
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
    bool recover_reset{true};
    unsigned read_timeout_ms{5000};
};

} // namespace

int main(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                usage_error(std::string{"missing value for "} + name + " (argument " + arg + ")");
            }
            return std::string{argv[++i]};
        };
        if (arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "--port") {
            options.port = next("--port");
        } else if (arg == "--pre-vid") {
            if (!parse_hex16(next("--pre-vid"), options.vendor)) {
                usage_error("invalid --pre-vid");
            }
        } else if (arg == "--pre-pid") {
            if (!parse_hex16(next("--pre-pid"), options.product)) {
                usage_error("invalid --pre-pid");
            }
        } else if (arg == "--identity-file") {
            options.identity_file = next("--identity-file");
        } else if (arg == "--read-timeout-ms") {
            if (!probe::parse_timeout_ms(next("--read-timeout-ms"), options.read_timeout_ms)) {
                usage_error("invalid --read-timeout-ms (plain decimal 1..300000 only)");
            }
        } else if (arg == "--no-recover-reset") {
            options.recover_reset = false;
        } else {
            usage_error("unknown argument: " + arg);
        }
    }

    // Gate P2-3: the port grammar is checked BEFORE any filesystem access;
    // an invalid port never reaches the sysfs seam and is never echoed raw.
    if (!probe::valid_port_context(options.port)) {
        usage_error("invalid --port (expected kernel port path like 1-2.1)");
    }
    if (options.identity_file.empty()) {
        usage_error("--identity-file is required (the only admission mode)");
    }

    aa::trust::WiredIdentity recorded{};
    std::string identity_error;
    if (!load_identity_file(options.identity_file, recorded, identity_error)) {
        usage_error(identity_error);
    }
    tr::WiredAdmission admit = admit_record(recorded);

    tr::UsbDeviceSelector selector;
    selector.port_context = options.port;
    selector.vendor_id = options.vendor;
    selector.product_id = options.product;

    probe::RealSysfsReader reader{options.port};
    const probe::SysfsSnapshot sysfs_before = probe::read_sysfs(options.port, reader);
    std::cout << "{\n";
    std::cout << "\"probe\": \"aa-usb-aoa-probe/task-28/2\",\n";
    std::cout << "\"port\": " << probe::json_string(options.port) << ",\n";
    std::cout << "\"admit_mode\": \"file\",\n";
    std::cout << "\"sysfs_before\": " << probe::sysfs_snapshot_json(sysfs_before) << ",\n";

    // Recovery: a device left in accessory mode by an earlier run gets one
    // targeted software reset so the full switch can be observed again. The
    // PORT is observed unfiltered: the phone-id selector would hide a device
    // already left in accessory mode.
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
        // Pin the reset to the OBSERVED device identity: a replacement device
        // that appears on the port between this observation and the reset must
        // never be reset (fix round P3).
        tr::UsbDeviceSelector pinned = port_only;
        pinned.bus = observed.value().bus;
        pinned.address = observed.value().address;
        pinned.vendor_id = observed.value().vendor_id;
        pinned.product_id = observed.value().product_id;
        auto reset = tr::reset_port_device(pinned);
        std::string reset_status = "ok";
        if (!reset) {
            // A software reset re-enumerates the device, so libusb may report
            // the dying handle as an error even when the reset worked: keep
            // waiting for phone mode either way and record what was seen.
            reset_status = aa::to_string(reset.error().code());
        }
        std::cout << "\"recovery_reset_status\": " << probe::json_string(reset_status) << ",\n";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            auto again = tr::observe_usb_device(port_only);
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
        return 3;
    }

    const tr::UsbAoaLink &record = link.value();
    std::cout << "\"connect_elapsed_ms\": " << connect_elapsed.count() << ",\n";
    std::cout << "\"switch\": {\n";
    std::cout << "  \"protocol_version\": " << record.switch_report.protocol_version << ",\n";
    std::cout << "  \"started\": " << (record.switch_report.started ? "true" : "false") << ",\n";
    std::cout << "  \"requests\": " << record.switch_report.requests << ",\n";
    std::cout << "  \"failed_at\": " << probe::json_string(record.switch_report.failed_at) << ",\n";
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
    std::cout << "\"identity_redacted\": "
              << probe::json_string(
                     aa::trust::redacted_label(aa::trust::TransportIdentity{record.identity}))
              << ",\n";

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
        std::cout << "\"transport_error\": "
                  << probe::json_string(aa::to_string(opened.error().code())) << ",\n";
        std::cout << "\"result\": \"transport-failed\"\n}\n";
        return 3;
    }
    std::cout << "\"transport_kind\": \""
              << (transport.value()->kind() == tr::Kind::usb_aoa ? "usb_aoa" : "other") << "\",\n";

    // Gate P2-4: the cancellation watchdog is armed BEFORE the framed send, so
    // a stalled OUT endpoint cannot hang the probe past the read timeout. It
    // wakes on completion (condition signaled once I/O returns) or on the
    // deadline, whichever comes first; join() then never waits out the timeout
    // after a completed round-trip.
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;
    std::thread watchdog([&source, &options, &done_mutex, &done_cv, &done] {
        std::unique_lock<std::mutex> lock(done_mutex);
        if (!done_cv.wait_for(lock, std::chrono::milliseconds(options.read_timeout_ms),
                              [&done] { return done; })) {
            source.request_stop();
        }
    });

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

    auto received = transport.value()->receive();
    {
        std::lock_guard<std::mutex> lock(done_mutex);
        done = true;
    }
    done_cv.notify_all();
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
    probe::RealSysfsReader after_reader{options.port};
    const probe::SysfsSnapshot sysfs_after = probe::read_sysfs(options.port, after_reader);
    std::cout << "\"sysfs_after\": " << probe::sysfs_snapshot_json(sysfs_after) << ",\n";

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
