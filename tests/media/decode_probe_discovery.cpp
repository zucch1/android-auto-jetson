#include "discovery.hpp"
#include <gst/gst.h>
#include <iostream>

int main() {
    gst_init(nullptr, nullptr);
    using namespace aa::decode_probe;
    int failures = 0;
    const auto unknown = discover_decoders("invented_nv_decoder");
    if (unknown.candidates.front().is_hardware) ++failures;
    const auto forced = discover_decoders("nvh264dec");
    if (!forced.candidates.front().is_hardware) ++failures;
    const auto automatic = discover_decoders();
    bool found = false;
    for (const auto& candidate : automatic.candidates) {
        if (candidate.element_name == "nvh264dec") {
            found = true;
            if (!candidate.is_hardware) ++failures;
        }
    }
    if (!found) ++failures;
    std::cout << "discovery failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
