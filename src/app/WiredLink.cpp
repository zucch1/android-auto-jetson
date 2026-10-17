// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WiredLink.hpp>

#include <aa/transport/UsbByteTransport.hpp>

#include <utility>

namespace aa::app {

core::Result<trust::PhoneIdentity> wired_phone_identity(const trust::WiredIdentity &identity) {
    const auto key = trust::validated_key(trust::TransportIdentity{identity});
    if (!key) {
        return key.error();
    }
    return trust::PhoneIdentity{key.value()};
}

core::Result<WiredLink> open_wired_link(const transport::UsbDeviceSelector &selector,
                                        const transport::AoaAccessoryInfo &info,
                                        const trust::TrustStore &store,
                                        const core::Deadline &deadline,
                                        trust::WiredIdentity *attempted_identity) {
    // Admission is the store's decision (admit_via); nothing is auto-approved.
    core::DeadlineCancellation bounded{deadline};
    auto link = transport::connect_usb_aoa(selector, info, transport::admit_via(store),
                                          core::Deadline{deadline.until, bounded.token()},
                                          attempted_identity);
    if (auto checked = deadline.check(); !checked) {
        return checked.error();
    }
    if (!link) {
        return link.error();
    }
    auto phone = wired_phone_identity(link.value().identity);
    if (!phone) {
        return phone.error();
    }
    auto framed = transport::UsbByteTransport::over_endpoint(std::move(link.value().endpoint));
    if (!framed) {
        return framed.error();
    }
    return WiredLink{std::move(framed.value()), phone.value()};
}

} // namespace aa::app
