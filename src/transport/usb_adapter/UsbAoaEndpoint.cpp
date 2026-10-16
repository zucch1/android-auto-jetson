// SPDX-License-Identifier: GPL-3.0-or-later
// AOA bulk endpoint and connection pipeline (task 28). See UsbAoaEndpoint.hpp
// and the error mapping in aa/transport/UsbAoa.hpp.

#include "UsbAoaEndpoint.hpp"

#include "LibusbWorld.hpp"

#include <utility>

namespace aa::transport {
namespace usb_adapter {

UsbAoaEndpoint::UsbAoaEndpoint(std::unique_ptr<AoaLinkFactory> factory, UsbDeviceSelector selector,
                               AoaAccessoryInfo info, WiredAdmission admit) noexcept
    : factory_(std::move(factory)), selector_(std::move(selector)), info_(std::move(info)),
      admit_(std::move(admit)) {}

UsbAoaEndpoint::~UsbAoaEndpoint() { close(); }

core::Result<std::unique_ptr<UsbAoaEndpoint>>
UsbAoaEndpoint::make(std::unique_ptr<AoaLinkFactory> factory, UsbDeviceSelector selector,
                     AoaAccessoryInfo info, WiredAdmission admit) {
    if (!factory) {
        return Error{ErrorCode::invalid_argument};
    }
    return std::unique_ptr<UsbAoaEndpoint>(new UsbAoaEndpoint(
        std::move(factory), std::move(selector), std::move(info), std::move(admit)));
}

core::Result<void> UsbAoaEndpoint::open() {
    if (opened_) {
        return {};
    }
    if (terminal_) {
        return Error{ErrorCode::transport_closed};
    }
    auto validated = validate_selector(selector_);
    if (!validated) {
        terminal_ = true;
        return validated.error();
    }
    auto switched = factory_->switch_to_accessory(selector_, info_, pre_switch_, switch_report_);
    if (!switched) {
        terminal_ = true;
        return switched.error();
    }
    auto pipe = factory_->claim_accessory(selector_, post_switch_);
    if (!pipe) {
        terminal_ = true;
        return pipe.error();
    }
    // AOA enumeration success is never trust: the identity captured from BOTH
    // descriptor observations must pass admission before the pipe is usable.
    identity_ = build_wired_identity(pre_switch_, post_switch_, selector_.port_context);
    auto admitted = admit_wired_identity(identity_, admit_);
    if (!admitted) {
        pipe.value()->close();
        terminal_ = true;
        return admitted.error();
    }
    pipe_ = std::move(pipe.value());
    opened_ = true;
    return {};
}

core::Result<std::size_t> UsbAoaEndpoint::read(std::span<std::byte> buffer,
                                               const core::CancellationToken &token) {
    if (!opened_ || terminal_ || !pipe_) {
        return Error{ErrorCode::transport_closed};
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    // Gate P2-4 read audit: unlike write, a zero-progress IN timeout is the
    // NORMAL quiet-channel case (waiting for the phone), so it must NOT count
    // toward a stall bound - a bound here would fail healthy idle channels and
    // race the cancellation contract. This wait is interruptible and its bound
    // is the cancellation token (ByteEndpoint.hpp), one kBulkTransferTimeoutMs
    // slice at a time; write carries the zero-progress stall bound instead.
    for (;;) {
        if (token.stop_requested()) {
            close();
            return Error{ErrorCode::cancelled};
        }
        auto count = pipe_->read(buffer, kBulkTransferTimeoutMs);
        if (!count) {
            close();
            return count.error();
        }
        if (count.value() > 0) {
            return count.value();
        }
    }
}

core::Result<std::size_t> UsbAoaEndpoint::write(std::span<const std::byte> data,
                                                const core::CancellationToken &token) {
    if (!opened_ || terminal_ || !pipe_) {
        return Error{ErrorCode::transport_closed};
    }
    if (data.empty()) {
        return std::size_t{0};
    }
    // Bounded stall policy (gate P2-4): each bulk attempt is already bounded by
    // kBulkTransferTimeoutMs; three consecutive zero-progress attempts mean the
    // OUT pipe is stuck and end in a typed transport_io failure instead of an
    // indefinite retry. Any failure terminally closes the endpoint.
    constexpr int kMaxZeroProgressWrites = 3;
    int zero_progress = 0;
    for (;;) {
        if (token.stop_requested()) {
            close();
            return Error{ErrorCode::cancelled};
        }
        auto count = pipe_->write(data, kBulkTransferTimeoutMs);
        if (!count) {
            close();
            return count.error();
        }
        if (count.value() > 0) {
            return count.value();
        }
        if (++zero_progress >= kMaxZeroProgressWrites) {
            close();
            return Error{ErrorCode::transport_io};
        }
    }
}

void UsbAoaEndpoint::close() noexcept {
    terminal_ = true;
    opened_ = false;
    if (pipe_) {
        pipe_->close();
    }
}

trust::WiredIdentity build_wired_identity(const UsbDeviceSnapshot &pre_switch,
                                          const UsbDeviceSnapshot &post_switch,
                                          const std::string &port_context) {
    trust::WiredIdentity identity;
    auto hex4 = [](std::uint16_t value) {
        char buffer[5] = {};
        const char *digits = "0123456789abcdef";
        buffer[0] = digits[(value >> 12U) & 0xFU];
        buffer[1] = digits[(value >> 8U) & 0xFU];
        buffer[2] = digits[(value >> 4U) & 0xFU];
        buffer[3] = digits[value & 0xFU];
        return std::string{buffer};
    };
    identity.usb_vendor_id = hex4(pre_switch.vendor_id);
    identity.usb_product_id = hex4(pre_switch.product_id);
    identity.usb_serial = pre_switch.serial;
    identity.aoa_manufacturer = post_switch.manufacturer;
    identity.aoa_model = post_switch.product;
    identity.aoa_serial = post_switch.serial;
    identity.port_context = port_context;
    return identity;
}

core::Result<void> admit_wired_identity(const trust::WiredIdentity &identity,
                                        const WiredAdmission &admit) {
    // Fail closed on unusable identity material before any trust question.
    const auto key = trust::validated_key(trust::TransportIdentity{identity});
    if (!key) {
        return key.error();
    }
    if (!admit) {
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    if (!admit(identity)) {
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    return {};
}

core::Result<UsbAoaLink> connect_aoa(std::unique_ptr<AoaLinkFactory> factory,
                                     const UsbDeviceSelector &selector,
                                     const AoaAccessoryInfo &info, const WiredAdmission &admit) {
    auto endpoint = UsbAoaEndpoint::make(std::move(factory), selector, info, admit);
    if (!endpoint) {
        return endpoint.error();
    }
    auto opened = endpoint.value()->open();
    if (!opened) {
        return opened.error();
    }
    UsbAoaLink link;
    link.identity = endpoint.value()->identity();
    link.pre_switch = endpoint.value()->pre_switch();
    link.post_switch = endpoint.value()->post_switch();
    link.switch_report = endpoint.value()->switch_report();
    link.endpoint = std::move(endpoint.value());
    return link;
}

} // namespace usb_adapter

WiredAdmission admit_via(const trust::TrustStore &store) {
    return [&store](const trust::WiredIdentity &identity) {
        const auto key = trust::validated_key(trust::TransportIdentity{identity});
        if (!key) {
            return false;
        }
        return trust::allows_session(store.lookup(trust::PhoneIdentity{key.value()}));
    };
}

core::Result<UsbAoaLink> connect_usb_aoa(const UsbDeviceSelector &selector,
                                         const AoaAccessoryInfo &info,
                                         const WiredAdmission &admit) {
    auto world = usb_adapter::make_libusb_link_factory();
    if (!world) {
        return world.error();
    }
    return usb_adapter::connect_aoa(std::move(world.value()), selector, info, admit);
}

core::Result<std::unique_ptr<ByteEndpoint>>
create_usb_aoa_endpoint(const UsbDeviceSelector &selector, const AoaAccessoryInfo &info,
                        const WiredAdmission &admit) {
    auto world = usb_adapter::make_libusb_link_factory();
    if (!world) {
        return world.error();
    }
    auto endpoint =
        usb_adapter::UsbAoaEndpoint::make(std::move(world.value()), selector, info, admit);
    if (!endpoint) {
        return endpoint.error();
    }
    return std::unique_ptr<ByteEndpoint>(std::move(endpoint.value()));
}

} // namespace aa::transport
