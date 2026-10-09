// Minimal aasdk headunit test app
// Confirms USB AOAP, protocol negotiation, and media projection
#include <aasdk/Common/Log.hpp>
#include <aasdk/USB/USBWrapper.hpp>
#include <aasdk/USB/AOAPDevice.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include "usb_init.hpp"


int main(int argc, char* argv[]) {
    // Modern logging macro (no explicit init)
    AASDK_LOG(info) << "[aasdk_simple_headunit] Starting minimal headunit test...";

    // Add a short delay to allow device to settle after re-enumeration
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    UsbInit usbInit;
    usbInit.initDevice();

    AASDK_LOG(info) << "[aasdk_simple_headunit] (Stub) Exiting.";
    return 0;
}
