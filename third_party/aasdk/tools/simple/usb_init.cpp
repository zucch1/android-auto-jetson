#include "usb_init.hpp"

UsbInit::UsbInit()
    : usbContext_(nullptr, &libusb_exit)
{
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        std::cerr << "[UsbInit] Failed to initialize libusb." << std::endl;
        return;
    }
    usbContext_.reset(ctx);
    usbWrapper_ = std::make_unique<aasdk::usb::USBWrapper>(usbContext_.get());
}
#include <cstring>
#include <libusb.h>
#include <thread>
#include <chrono>

// AOAP control request constants

// Helper to send AOAP string
// AOAP control request constants
#define ACCESSORY_GET_PROTOCOL 51
#define ACCESSORY_SEND_STRING 52
#define ACCESSORY_START 53
#define ACCESSORY_STRING_MANUFACTURER 0
#define ACCESSORY_STRING_MODEL 1
#define ACCESSORY_STRING_DESCRIPTION 2
#define ACCESSORY_STRING_VERSION 3
#define ACCESSORY_STRING_URI 4
#define ACCESSORY_STRING_SERIAL 5
// Helper to send AOAP string
int send_aoap_string(libusb_device_handle* handle, int index, const char* str) {
    return libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        ACCESSORY_SEND_STRING,
        0,
        index,
        (unsigned char*)str,
        strlen(str) + 1,
        1000);
}
#include "usb_init.hpp"
#include <aasdk/USB/AccessoryModeQueryType.hpp>
#include <aasdk/USB/AOAPDevice.hpp>

void UsbInit::initDevice() {
    std::cout << "[UsbInit] Starting AOAP handshake..." << std::endl;

    // Enumerate all USB devices and try AOAP handshake on each Google device
    bool found = false;
    bool switched = false;
    int attempts = 0;
    while (attempts < 2) {
        libusb_device **devs = nullptr;
        ssize_t cnt = libusb_get_device_list(usbContext_.get(), &devs);
        if (cnt < 0) {
            std::cerr << "[UsbInit] Failed to get USB device list." << std::endl;
            return;
        }
        for (ssize_t i = 0; i < cnt; ++i) {
            libusb_device *dev = devs[i];
            libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(dev, &desc) == 0) {
                if (desc.idVendor == 0x18D1) {
                    std::cout << "[UsbInit] Found Google device: VID 0x" << std::hex << desc.idVendor << ", PID 0x" << desc.idProduct << std::dec << std::endl;
                    aasdk::usb::DeviceHandle handle;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    bool isAOAP = (desc.idProduct >= 0x2D00 && desc.idProduct <= 0x2D05);
                    if (isAOAP || attempts == 0) {
                        if (usbWrapper_->open(dev, handle) == 0 && handle) {
                            found = true;
                            if (isAOAP) {
                                std::cout << "[UsbInit] Device already in accessory mode (PID 0x" << std::hex << desc.idProduct << ")." << std::dec << std::endl;
                                aasdk::usb::AccessoryModeQueryFactory queryFactory(*usbWrapper_, ioService_);
                                aasdk::usb::AccessoryModeQueryChainFactory chainFactory(*usbWrapper_, ioService_, queryFactory);
                                auto queryChain = chainFactory.create();
                                using DeviceHandle = aasdk::usb::DeviceHandle;
                                auto promise = aasdk::usb::IAccessoryModeQueryChain::Promise::defer(ioService_);
                                std::cout << "[UsbInit] AOAP handshake complete! Device ready." << std::endl;
                                queryChain->start(handle, promise);
                                ioService_.run();
                                libusb_free_device_list(devs, 1);
                                return;
                            } else if (!switched) {
                                std::cout << "[UsbInit] Device not in accessory mode, attempting AOAP switch..." << std::endl;
                                libusb_device_handle* raw_handle = nullptr;
                                if (libusb_open(dev, &raw_handle) == 0 && raw_handle) {
                                    std::cout << "[UsbInit] Opened device for AOAP switching." << std::endl;
                                    unsigned char io_buf[2] = {0};
                                    int protocol = libusb_control_transfer(raw_handle,
                                        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
                                        ACCESSORY_GET_PROTOCOL,
                                        0, 0, io_buf, 2, 1000);
                                    if (protocol >= 2) {
                                        int version = io_buf[1] << 8 | io_buf[0];
                                        std::cout << "[UsbInit] AOAP protocol version: " << version << std::endl;
                                    } else {
                                        std::cerr << "[UsbInit] Failed to get AOAP protocol version (ret=" << protocol << ")." << std::endl;
                                    }
                                    if (send_aoap_string(raw_handle, ACCESSORY_STRING_MANUFACTURER, "Google, Inc.") >= 0)
                                        std::cout << "[UsbInit] Sent manufacturer string." << std::endl;
                                    else
                                        std::cerr << "[UsbInit] Failed to send manufacturer string." << std::endl;
                                    if (send_aoap_string(raw_handle, ACCESSORY_STRING_MODEL, "Android Auto") >= 0)
                                        std::cout << "[UsbInit] Sent model string." << std::endl;
                                    else
                                        std::cerr << "[UsbInit] Failed to send model string." << std::endl;
                                    if (send_aoap_string(raw_handle, ACCESSORY_STRING_DESCRIPTION, "Android Auto Headunit") >= 0)
                                        std::cout << "[UsbInit] Sent description string." << std::endl;
                                    else
                                        std::cerr << "[UsbInit] Failed to send description string." << std::endl;
                                    if (send_aoap_string(raw_handle, ACCESSORY_STRING_VERSION, "1.0") >= 0)
                                        std::cout << "[UsbInit] Sent version string." << std::endl;
                                    else
                                        std::cerr << "[UsbInit] Failed to send version string." << std::endl;
                                    if (send_aoap_string(raw_handle, ACCESSORY_STRING_URI, "") >= 0)
                                        std::cout << "[UsbInit] Sent URI string." << std::endl;
                                    else
                                        std::cerr << "[UsbInit] Failed to send URI string." << std::endl;
                                    if (send_aoap_string(raw_handle, ACCESSORY_STRING_SERIAL, "serial") >= 0)
                                        std::cout << "[UsbInit] Sent serial string." << std::endl;
                                    else
                                        std::cerr << "[UsbInit] Failed to send serial string." << std::endl;
                                    int start = libusb_control_transfer(raw_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        ACCESSORY_START,
                                        0, 0, nullptr, 0, 1000);
                                    if (start >= 0) {
                                        std::cout << "[UsbInit] Sent ACCESSORY_START, device should re-enumerate." << std::endl;
                                        switched = true;
                                    } else {
                                        std::cerr << "[UsbInit] Failed to send ACCESSORY_START." << std::endl;
                                    }
                                    libusb_close(raw_handle);
                                } else {
                                    std::cerr << "[UsbInit] Could not open device for AOAP switching." << std::endl;
                                }
                            }
                        } else {
                            std::cerr << "[UsbInit] Could not open device PID 0x" << std::hex << desc.idProduct << std::dec << std::endl;
                        }
                    }
                }
            }
        }
        libusb_free_device_list(devs, 1);
        if (switched && attempts == 0) {
            std::cout << "[UsbInit] Waiting for device to re-enumerate in accessory mode..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            // Retry loop: scan for AOAP device every 300ms for up to 2 seconds
            bool aoap_found = false;
            for (int retry = 0; retry < 7; ++retry) {
                libusb_device **retry_devs = nullptr;
                ssize_t retry_cnt = libusb_get_device_list(usbContext_.get(), &retry_devs);
                if (retry_cnt > 0) {
                    for (ssize_t j = 0; j < retry_cnt; ++j) {
                        libusb_device *retry_dev = retry_devs[j];
                        libusb_device_descriptor retry_desc;
                        if (libusb_get_device_descriptor(retry_dev, &retry_desc) == 0) {
                            if (retry_desc.idVendor == 0x18D1 && retry_desc.idProduct >= 0x2D00 && retry_desc.idProduct <= 0x2D05) {
                                std::cout << "[UsbInit] AOAP device detected after switch: PID 0x" << std::hex << retry_desc.idProduct << std::dec << std::endl;
                                aasdk::usb::DeviceHandle retry_handle;
                                if (usbWrapper_->open(retry_dev, retry_handle) == 0 && retry_handle) {
                                    found = true;
                                    std::cout << "[UsbInit] Device now in accessory mode (PID 0x" << std::hex << retry_desc.idProduct << ")." << std::dec << std::endl;
                                    aasdk::usb::AccessoryModeQueryFactory queryFactory(*usbWrapper_, ioService_);
                                    aasdk::usb::AccessoryModeQueryChainFactory chainFactory(*usbWrapper_, ioService_, queryFactory);
                                    auto queryChain = chainFactory.create();
                                    using DeviceHandle = aasdk::usb::DeviceHandle;
                                    auto promise = aasdk::usb::IAccessoryModeQueryChain::Promise::defer(ioService_);
                                    std::cout << "[UsbInit] AOAP handshake complete! Device ready." << std::endl;
                                    queryChain->start(retry_handle, promise);
                                    ioService_.run();
                                    libusb_free_device_list(retry_devs, 1);
                                    return;
                                }
                            }
                        }
                    }
                    libusb_free_device_list(retry_devs, 1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            if (!found) {
                std::cerr << "[UsbInit] AOAP device did not appear after switching." << std::endl;
            }
            ++attempts;
            continue;
        }
        break;
    }
    if (!found) {
        std::cerr << "[UsbInit] No Google Android device found (VID 0x18D1)." << std::endl;
    }
}
