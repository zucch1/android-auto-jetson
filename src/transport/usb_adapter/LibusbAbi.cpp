// SPDX-License-Identifier: GPL-3.0-or-later
// dlopen-backed libusb-1.0 ABI binding (task-28 linkage decision). See
// LibusbAbi.hpp: no libusb header or import library is required to build this
// adapter on host or cross; the runtime .so is resolved on the target.

#include "LibusbAbi.hpp"

#include <dlfcn.h>

#include <utility>

namespace aa::transport::usb_adapter {
namespace {

// Resolve one ABI entry point; null on a missing symbol so load() fails closed.
template <class Fn> void resolve(void *library, const char *name, Fn &out) {
    out = reinterpret_cast<Fn>(::dlsym(library, name));
}

} // namespace

LibusbApi::~LibusbApi() {
    if (library_ != nullptr) {
        ::dlclose(library_);
        library_ = nullptr;
    }
}

LibusbApi::LibusbApi(LibusbApi &&other) noexcept { *this = std::move(other); }

LibusbApi &LibusbApi::operator=(LibusbApi &&other) noexcept {
    if (this != &other) {
        if (library_ != nullptr) {
            ::dlclose(library_);
        }
        library_ = other.library_;
        other.library_ = nullptr;
        init = std::exchange(other.init, nullptr);
        exit = std::exchange(other.exit, nullptr);
        get_device_list = std::exchange(other.get_device_list, nullptr);
        free_device_list = std::exchange(other.free_device_list, nullptr);
        get_device_descriptor = std::exchange(other.get_device_descriptor, nullptr);
        get_bus_number = std::exchange(other.get_bus_number, nullptr);
        get_device_address = std::exchange(other.get_device_address, nullptr);
        get_port_numbers = std::exchange(other.get_port_numbers, nullptr);
        open = std::exchange(other.open, nullptr);
        close = std::exchange(other.close, nullptr);
        get_string_descriptor_ascii = std::exchange(other.get_string_descriptor_ascii, nullptr);
        claim_interface = std::exchange(other.claim_interface, nullptr);
        release_interface = std::exchange(other.release_interface, nullptr);
        control_transfer = std::exchange(other.control_transfer, nullptr);
        bulk_transfer = std::exchange(other.bulk_transfer, nullptr);
        reset_device = std::exchange(other.reset_device, nullptr);
        kernel_driver_active = std::exchange(other.kernel_driver_active, nullptr);
        detach_kernel_driver = std::exchange(other.detach_kernel_driver, nullptr);
    }
    return *this;
}

core::Result<LibusbApi> LibusbApi::load() {
    // The runtime SONAME on both the target and hosts with libusb-1.0.
    void *library = ::dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        library = ::dlopen("libusb-1.0.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (library == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    LibusbApi api;
    api.library_ = library;
    resolve(library, "libusb_init", api.init);
    resolve(library, "libusb_exit", api.exit);
    resolve(library, "libusb_get_device_list", api.get_device_list);
    resolve(library, "libusb_free_device_list", api.free_device_list);
    resolve(library, "libusb_get_device_descriptor", api.get_device_descriptor);
    resolve(library, "libusb_get_bus_number", api.get_bus_number);
    resolve(library, "libusb_get_device_address", api.get_device_address);
    resolve(library, "libusb_get_port_numbers", api.get_port_numbers);
    resolve(library, "libusb_open", api.open);
    resolve(library, "libusb_close", api.close);
    resolve(library, "libusb_get_string_descriptor_ascii", api.get_string_descriptor_ascii);
    resolve(library, "libusb_claim_interface", api.claim_interface);
    resolve(library, "libusb_release_interface", api.release_interface);
    resolve(library, "libusb_control_transfer", api.control_transfer);
    resolve(library, "libusb_bulk_transfer", api.bulk_transfer);
    resolve(library, "libusb_reset_device", api.reset_device);
    resolve(library, "libusb_kernel_driver_active", api.kernel_driver_active);
    resolve(library, "libusb_detach_kernel_driver", api.detach_kernel_driver);
    const bool complete =
        api.init != nullptr && api.exit != nullptr && api.get_device_list != nullptr &&
        api.free_device_list != nullptr && api.get_device_descriptor != nullptr &&
        api.get_bus_number != nullptr && api.get_device_address != nullptr &&
        api.get_port_numbers != nullptr && api.open != nullptr && api.close != nullptr &&
        api.get_string_descriptor_ascii != nullptr && api.claim_interface != nullptr &&
        api.release_interface != nullptr && api.control_transfer != nullptr &&
        api.bulk_transfer != nullptr && api.reset_device != nullptr &&
        api.kernel_driver_active != nullptr && api.detach_kernel_driver != nullptr;
    if (!complete) {
        // The destructor closes the library; no partial ABI is ever usable.
        return Error{ErrorCode::transport_unavailable};
    }
    return api;
}

Error map_libusb_error(int code) noexcept {
    switch (code) {
    case kLibusbErrorNoDevice:
        // The device went away (hotplug): the link is terminal, not merely sick.
        return Error{ErrorCode::transport_closed};
    case kLibusbErrorNotFound:
    case kLibusbErrorNotSupported:
        return Error{ErrorCode::transport_unavailable};
    case kLibusbErrorTimeout:
        return Error{ErrorCode::timeout};
    default:
        return Error{ErrorCode::transport_io};
    }
}

} // namespace aa::transport::usb_adapter
