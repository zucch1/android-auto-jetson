#pragma once
#include <aasdk/USB/USBWrapper.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryChain.hpp>
#include <aasdk/IO/Promise.hpp>
#include <boost/asio.hpp>
#include <libusb.h>
#include <memory>
#include <iostream>

class UsbInit {
public:
    UsbInit();
    void initDevice();
private:
    boost::asio::io_service ioService_;
    std::unique_ptr<libusb_context, decltype(&libusb_exit)> usbContext_;
    std::unique_ptr<aasdk::usb::USBWrapper> usbWrapper_;
};
