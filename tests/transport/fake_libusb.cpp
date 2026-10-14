// SPDX-License-Identifier: GPL-3.0-or-later
// Fake libusb-1.0 runtime for task-28 tests (pre-merge fix round). Built as
// libusb-1.0.so.0 and loaded through the adapter's real dlopen path
// (LD_LIBRARY_PATH on the test process), so the production LibusbWorld code -
// not a mock of it - runs against a scriptable device. Pure test harness: it
// never ships, and no production source depends on it.
//
// Scenario driver (env, read per call so one process can switch per test):
//   AA_FAKE_LIBUSB_SCENARIO  absent (default) | happy | stall |
//                            claim-fail-pre | claim-fail-always | replacement
//   AA_FAKE_LIBUSB_LOG       call-log file path (appended), one call per line
//
// Device model: one device on port path "1-2.1" (bus 1). Phone mode is
// 04e8:6860 with fixed strings; the AOA START control request re-enumerates it
// as accessory 18d1:2d00 whose strings echo the SEND_STRING slots (as real AOA
// accessory mode does). "replacement" is a non-matching gadget (1234:5678)
// that must never be touched by a phone-id selector.

#include "../../src/transport/usb_adapter/LibusbAbi.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

namespace ua = aa::transport::usb_adapter;

enum class Mode { phone, accessory };

Mode g_mode = Mode::phone;
char g_slots[6][256] = {};
std::mutex g_mutex;

struct FakeDevice final {
    int unused;
};
FakeDevice g_device_storage;
struct FakeHandle final {
    int unused;
};

[[nodiscard]] const char *scenario_text() {
    const char *value = ::getenv("AA_FAKE_LIBUSB_SCENARIO");
    return value == nullptr ? "" : value;
}

enum class Scenario { absent, happy, stall, claim_fail_pre, claim_fail_always, replacement };

[[nodiscard]] Scenario scenario() {
    const std::string text{scenario_text()};
    if (text == "happy") {
        return Scenario::happy;
    }
    if (text == "stall") {
        return Scenario::stall;
    }
    if (text == "claim-fail-pre") {
        return Scenario::claim_fail_pre;
    }
    if (text == "claim-fail-always") {
        return Scenario::claim_fail_always;
    }
    if (text == "replacement") {
        return Scenario::replacement;
    }
    return Scenario::absent;
}

void log_line(const char *line) {
    const char *path = ::getenv("AA_FAKE_LIBUSB_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::FILE *out = std::fopen(path, "a");
    if (out == nullptr) {
        return;
    }
    std::fputs(line, out);
    std::fputc('\n', out);
    std::fclose(out);
}

void log_control(std::uint8_t request, std::uint16_t index) {
    char line[64] = {};
    std::snprintf(line, sizeof(line), "libusb_control_transfer:%u:%u",
                  static_cast<unsigned>(request), static_cast<unsigned>(index));
    log_line(line);
}

void log_bulk(std::uint8_t endpoint) {
    char line[64] = {};
    std::snprintf(line, sizeof(line), "libusb_bulk_transfer:%02x", static_cast<unsigned>(endpoint));
    log_line(line);
}

// ABI-identical strong scalar types for libusb_control_transfer's parameters.
// The libusb C signature puts mutually convertible scalars side by side
// (bmRequestType/bRequest, wValue/wIndex, wLength/timeout); distinct enum
// types keep the exact ABI layout while making a parameter swap a type error
// (bugprone-easily-swappable-parameters).
enum class BmRequestType : std::uint8_t {};
enum class BRequest : std::uint8_t {};
enum class WValue : std::uint16_t {};
enum class WIndex : std::uint16_t {};
enum class WLength : std::uint16_t {};
enum class TimeoutMs : unsigned {};

// A valid AAP bulk reply frame (channel 0, bulk/specific/plain, 6-byte
// VersionResponse-shaped payload) so probe receive() completes immediately.
constexpr std::uint8_t kReplyFrame[] = {0x00, 0x03, 0x00, 0x06, 0x00, 0x02, 0x00, 0x01, 0x00, 0x06};

[[nodiscard]] bool claim_refused(const Mode mode) {
    switch (scenario()) {
    case Scenario::claim_fail_always:
        return true;
    case Scenario::claim_fail_pre:
        return mode == Mode::phone;
    default:
        return false;
    }
}

// Only the pre-switch refusal scenario reports a bound kernel driver, so the
// detach failure path is exercised there while claim-fail-always exercises the
// claim_interface failure directly.
[[nodiscard]] bool kernel_driver_bound(const Mode mode) {
    return scenario() == Scenario::claim_fail_pre && mode == Mode::phone;
}

} // namespace

extern "C" {

int libusb_init(ua::LibusbContext **ctx) {
    std::lock_guard lock(g_mutex);
    log_line("libusb_init");
    g_mode = Mode::phone;
    std::memset(g_slots, 0, sizeof(g_slots));
    *ctx = reinterpret_cast<ua::LibusbContext *>(&g_device_storage);
    return ua::kLibusbSuccess;
}

void libusb_exit(ua::LibusbContext *ctx) {
    (void)ctx;
    log_line("libusb_exit");
}

std::ptrdiff_t libusb_get_device_list(ua::LibusbContext *ctx, ua::LibusbDevice ***list) {
    (void)ctx;
    std::lock_guard lock(g_mutex);
    log_line("libusb_get_device_list");
    if (scenario() == Scenario::absent) {
        *list = static_cast<ua::LibusbDevice **>(std::calloc(1, sizeof(ua::LibusbDevice *)));
        return 0;
    }
    auto **entries = static_cast<ua::LibusbDevice **>(std::calloc(2, sizeof(ua::LibusbDevice *)));
    entries[0] = reinterpret_cast<ua::LibusbDevice *>(&g_device_storage);
    entries[1] = nullptr;
    *list = entries;
    return 1;
}

void libusb_free_device_list(ua::LibusbDevice **list, int unref_devices) {
    (void)unref_devices;
    std::free(list);
}

int libusb_get_device_descriptor(ua::LibusbDevice *dev, ua::LibusbDeviceDescriptor *desc) {
    (void)dev;
    std::lock_guard lock(g_mutex);
    *desc = ua::LibusbDeviceDescriptor{};
    desc->bLength = 18;
    desc->bDescriptorType = 1;
    desc->bMaxPacketSize0 = 64;
    desc->iManufacturer = 1;
    desc->iProduct = 2;
    desc->iSerialNumber = 3;
    switch (scenario()) {
    case Scenario::replacement:
        desc->idVendor = 0x1234;
        desc->idProduct = 0x5678;
        break;
    default:
        if (g_mode == Mode::accessory) {
            desc->idVendor = 0x18D1;
            desc->idProduct = 0x2D00;
        } else {
            desc->idVendor = 0x04E8;
            desc->idProduct = 0x6860;
        }
        break;
    }
    return ua::kLibusbSuccess;
}

std::uint8_t libusb_get_bus_number(ua::LibusbDevice *dev) {
    (void)dev;
    return 1;
}

std::uint8_t libusb_get_device_address(ua::LibusbDevice *dev) {
    (void)dev;
    return 7;
}

int libusb_get_port_numbers(ua::LibusbDevice *dev, std::uint8_t *ports, int ports_length) {
    (void)dev;
    if (ports_length < 2) {
        return ua::kLibusbErrorIo;
    }
    ports[0] = 2;
    ports[1] = 1;
    return 2;
}

int libusb_open(ua::LibusbDevice *dev, ua::LibusbDeviceHandle **handle) {
    (void)dev;
    log_line("libusb_open");
    *handle = reinterpret_cast<ua::LibusbDeviceHandle *>(std::malloc(sizeof(FakeHandle)));
    return ua::kLibusbSuccess;
}

void libusb_close(ua::LibusbDeviceHandle *handle) {
    log_line("libusb_close");
    std::free(handle);
}

int libusb_get_string_descriptor_ascii(ua::LibusbDeviceHandle *handle, std::uint8_t index,
                                       std::uint8_t *data, int length) {
    (void)handle;
    std::lock_guard lock(g_mutex);
    const char *text = "";
    if (scenario() == Scenario::replacement) {
        switch (index) {
        case 1:
            text = "ACME";
            break;
        case 2:
            text = "Gadget";
            break;
        case 3:
            text = "REPLACEMENT-001";
            break;
        default:
            break;
        }
    } else if (g_mode == Mode::accessory) {
        switch (index) {
        case 1:
            text = g_slots[0];
            break;
        case 2:
            text = g_slots[1];
            break;
        case 3:
            text = g_slots[5];
            break;
        default:
            break;
        }
    } else {
        switch (index) {
        case 1:
            text = "SAMSUNG";
            break;
        case 2:
            text = "SAMSUNG_Android";
            break;
        case 3:
            text = "TEST-SERIAL-001";
            break;
        default:
            break;
        }
    }
    const std::size_t size = std::strlen(text);
    if (size == 0 || length <= 0) {
        return ua::kLibusbErrorNotFound;
    }
    std::size_t count = size;
    if (count > static_cast<std::size_t>(length)) {
        count = static_cast<std::size_t>(length);
    }
    std::memcpy(data, text, count);
    return static_cast<int>(count);
}

int libusb_kernel_driver_active(ua::LibusbDeviceHandle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    std::lock_guard lock(g_mutex);
    return kernel_driver_bound(g_mode) ? 1 : 0;
}

int libusb_detach_kernel_driver(ua::LibusbDeviceHandle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    log_line("libusb_detach_kernel_driver");
    std::lock_guard lock(g_mutex);
    return claim_refused(g_mode) ? ua::kLibusbErrorAccess : ua::kLibusbSuccess;
}

int libusb_claim_interface(ua::LibusbDeviceHandle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    log_line("libusb_claim_interface");
    std::lock_guard lock(g_mutex);
    return claim_refused(g_mode) ? ua::kLibusbErrorAccess : ua::kLibusbSuccess;
}

int libusb_release_interface(ua::LibusbDeviceHandle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    log_line("libusb_release_interface");
    return ua::kLibusbSuccess;
}

int libusb_control_transfer(ua::LibusbDeviceHandle *handle, BmRequestType bm_request_type,
                            BRequest b_request, WValue w_value, WIndex w_index, std::uint8_t *data,
                            WLength w_length, TimeoutMs timeout_ms) {
    const auto request_type = static_cast<std::uint8_t>(bm_request_type);
    const auto request = static_cast<std::uint8_t>(b_request);
    const auto value = static_cast<std::uint16_t>(w_value);
    const auto index = static_cast<std::uint16_t>(w_index);
    const auto length = static_cast<std::uint16_t>(w_length);
    const unsigned timeout = static_cast<unsigned>(timeout_ms);
    (void)handle;
    (void)value;
    (void)timeout;
    log_control(request, index);
    std::lock_guard lock(g_mutex);
    if (scenario() == Scenario::replacement || scenario() == Scenario::absent) {
        return ua::kLibusbErrorNotFound;
    }
    if (request_type == (ua::kUsbTypeVendor | ua::kUsbEndpointIn) && request == 51) {
        if (length < 2) {
            return ua::kLibusbErrorIo;
        }
        data[0] = 0x02;
        data[1] = 0x00;
        return 2;
    }
    if (request_type == ua::kUsbTypeVendor && request == 52) {
        if (index < 6) {
            std::size_t count = static_cast<std::size_t>(length) < 255
                                    ? static_cast<std::size_t>(length)
                                    : std::size_t{255};
            while (count > 0 && data[count - 1] == 0) {
                --count;
            }
            std::memcpy(g_slots[index], data, count);
            g_slots[index][count] = '\0';
        }
        return static_cast<int>(length);
    }
    if (request_type == ua::kUsbTypeVendor && request == 53) {
        g_mode = Mode::accessory;
        return 0;
    }
    return ua::kLibusbErrorNotFound;
}

int libusb_bulk_transfer(ua::LibusbDeviceHandle *handle, std::uint8_t endpoint, std::uint8_t *data,
                         int length, int *transferred, unsigned timeout_ms) {
    (void)handle;
    (void)timeout_ms;
    log_bulk(endpoint);
    std::lock_guard lock(g_mutex);
    if (endpoint == 0x01) {
        *transferred = length;
        return ua::kLibusbSuccess;
    }
    if (endpoint == 0x81) {
        if (scenario() == Scenario::stall) {
            *transferred = 0;
            return ua::kLibusbErrorTimeout;
        }
        const int count = static_cast<int>(sizeof(kReplyFrame)) < length
                              ? static_cast<int>(sizeof(kReplyFrame))
                              : length;
        std::memcpy(data, kReplyFrame, static_cast<std::size_t>(count));
        *transferred = count;
        return ua::kLibusbSuccess;
    }
    *transferred = 0;
    return ua::kLibusbErrorNotFound;
}

int libusb_reset_device(ua::LibusbDeviceHandle *handle) {
    (void)handle;
    log_line("libusb_reset_device");
    return ua::kLibusbSuccess;
}

} // extern "C"
