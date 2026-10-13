// SPDX-License-Identifier: GPL-3.0-or-later
// Private adapter: a minimal libusb-1.0 C ABI binding loaded at run time with
// dlopen("libusb-1.0.so.0"). Only the entry points the AOA connection pipeline
// uses are declared, so this adapter compiles and links with NO libusb header
// and NO libusb import library: the aarch64 cross sysroot and the Jetson target
// carry runtime libusb only (task-28 linkage decision: dlopen, never link).
//
// The struct and function signatures below mirror the libusb-1.0 ABI exactly
// (libusb_device_descriptor is the 18-byte wire layout; function shapes match
// libusb.h 1.0.x). Nothing external crosses this adapter boundary.
#pragma once

#include <aa/core/Result.hpp>

#include <cstddef>
#include <cstdint>

namespace aa::transport::usb_adapter {

// libusb error codes used for typed error mapping.
inline constexpr int kLibusbSuccess = 0;
inline constexpr int kLibusbErrorIo = -1;
inline constexpr int kLibusbErrorAccess = -3;
inline constexpr int kLibusbErrorNoDevice = -4;
inline constexpr int kLibusbErrorNotFound = -5;
inline constexpr int kLibusbErrorBusy = -6;
inline constexpr int kLibusbErrorTimeout = -7;
inline constexpr int kLibusbErrorInterrupted = -10;
inline constexpr int kLibusbErrorNotSupported = -12;

// Control-transfer direction/type bits (bmRequestType) used by AOA.
inline constexpr std::uint8_t kUsbEndpointIn = 0x80;
inline constexpr std::uint8_t kUsbEndpointOut = 0x00;
inline constexpr std::uint8_t kUsbTypeVendor = 0x40;

// libusb_device_descriptor, ABI layout (18 bytes, no padding).
struct LibusbDeviceDescriptor final {
    std::uint8_t bLength{};
    std::uint8_t bDescriptorType{};
    std::uint16_t bcdUSB{};
    std::uint8_t bDeviceClass{};
    std::uint8_t bDeviceSubClass{};
    std::uint8_t bDeviceProtocol{};
    std::uint8_t bMaxPacketSize0{};
    std::uint16_t idVendor{};
    std::uint16_t idProduct{};
    std::uint16_t bcdDevice{};
    std::uint8_t iManufacturer{};
    std::uint8_t iProduct{};
    std::uint8_t iSerialNumber{};
    std::uint8_t bNumConfigurations{};
};
static_assert(sizeof(LibusbDeviceDescriptor) == 18, "libusb_device_descriptor ABI layout");

// Opaque ABI handles (real libusb_context / libusb_device / libusb_device_handle).
struct LibusbContext;
struct LibusbDevice;
struct LibusbDeviceHandle;

// Loaded libusb ABI: RAII over the dlopen handle. Function-pointer members are
// null until load() succeeds; move-only so exactly one owner closes the library.
class LibusbApi final {
  public:
    LibusbApi() = default;
    ~LibusbApi();
    LibusbApi(const LibusbApi &) = delete;
    LibusbApi &operator=(const LibusbApi &) = delete;
    LibusbApi(LibusbApi &&other) noexcept;
    LibusbApi &operator=(LibusbApi &&other) noexcept;

    // dlopen the runtime library and resolve every entry point used here.
    // Fails closed with transport_unavailable when the library or any symbol
    // is missing (the adapter never falls back to a linked libusb).
    [[nodiscard]] static core::Result<LibusbApi> load();

    [[nodiscard]] bool valid() const noexcept { return library_ != nullptr; }

    // libusb-1.0 entry points (ABI-exact shapes).
    int (*init)(LibusbContext **ctx) = nullptr;
    void (*exit)(LibusbContext *ctx) = nullptr;
    std::ptrdiff_t (*get_device_list)(LibusbContext *ctx, LibusbDevice ***list) = nullptr;
    void (*free_device_list)(LibusbDevice **list, int unref_devices) = nullptr;
    int (*get_device_descriptor)(LibusbDevice *dev, LibusbDeviceDescriptor *desc) = nullptr;
    std::uint8_t (*get_bus_number)(LibusbDevice *dev) = nullptr;
    std::uint8_t (*get_device_address)(LibusbDevice *dev) = nullptr;
    int (*get_port_numbers)(LibusbDevice *dev, std::uint8_t *ports, int ports_length) = nullptr;
    int (*open)(LibusbDevice *dev, LibusbDeviceHandle **handle) = nullptr;
    void (*close)(LibusbDeviceHandle *handle) = nullptr;
    int (*get_string_descriptor_ascii)(LibusbDeviceHandle *handle, std::uint8_t index,
                                       std::uint8_t *data, int length) = nullptr;
    int (*claim_interface)(LibusbDeviceHandle *handle, int interface_number) = nullptr;
    int (*release_interface)(LibusbDeviceHandle *handle, int interface_number) = nullptr;
    int (*control_transfer)(LibusbDeviceHandle *handle, std::uint8_t request_type,
                            std::uint8_t request, std::uint16_t value, std::uint16_t index,
                            std::uint8_t *data, std::uint16_t length,
                            unsigned timeout_ms) = nullptr;
    int (*bulk_transfer)(LibusbDeviceHandle *handle, std::uint8_t endpoint, std::uint8_t *data,
                         int length, int *transferred, unsigned timeout_ms) = nullptr;
    int (*reset_device)(LibusbDeviceHandle *handle) = nullptr;
    int (*kernel_driver_active)(LibusbDeviceHandle *handle, int interface_number) = nullptr;
    int (*detach_kernel_driver)(LibusbDeviceHandle *handle, int interface_number) = nullptr;

  private:
    void *library_{nullptr};
};

// RAII device handle. Closed exactly once; later close() calls are no-ops.
class LibusbHandle final {
  public:
    LibusbHandle() = default;
    LibusbHandle(const LibusbApi *api, LibusbDeviceHandle *handle) noexcept
        : api_(api), handle_(handle) {}
    ~LibusbHandle() { close(); }
    LibusbHandle(const LibusbHandle &) = delete;
    LibusbHandle &operator=(const LibusbHandle &) = delete;
    LibusbHandle(LibusbHandle &&other) noexcept : api_(other.api_), handle_(other.handle_) {
        other.handle_ = nullptr;
        other.api_ = nullptr;
    }
    LibusbHandle &operator=(LibusbHandle &&other) noexcept {
        if (this != &other) {
            close();
            api_ = other.api_;
            handle_ = other.handle_;
            other.handle_ = nullptr;
            other.api_ = nullptr;
        }
        return *this;
    }

    void close() noexcept {
        if (handle_ != nullptr && api_ != nullptr) {
            api_->close(handle_);
        }
        handle_ = nullptr;
    }

    [[nodiscard]] LibusbDeviceHandle *get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

  private:
    const LibusbApi *api_{nullptr};
    LibusbDeviceHandle *handle_{nullptr};
};

// Typed mapping of a libusb error code into the transport error band.
[[nodiscard]] Error map_libusb_error(int code) noexcept;

} // namespace aa::transport::usb_adapter
