// SPDX-License-Identifier: GPL-3.0-or-later
// USB byte-endpoint transport (task 16): complete AAP frames over any
// ByteEndpoint, with the shared bounded frame codec doing the framing. This is
// the seam task 28 fills with a real wired USB/AOA bulk endpoint; no libusb or
// hardware dependency appears here.

#include <aa/transport/UsbByteTransport.hpp>

#include "FrameStreamIO.hpp"

#include <utility>

namespace aa::transport {

struct UsbByteTransport::Impl final {
    std::unique_ptr<ByteEndpoint> endpoint;
    FrameStreamIO io;

    explicit Impl(std::unique_ptr<ByteEndpoint> ep) noexcept
        : endpoint(std::move(ep)),
          io([this](std::span<std::byte> buffer, const core::CancellationToken& token) {
              return this->endpoint->read(buffer, token);
          },
             [this](std::span<const std::byte> data, const core::CancellationToken& token) {
                 return this->endpoint->write(data, token);
             }) {}
};

UsbByteTransport::UsbByteTransport(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

UsbByteTransport::~UsbByteTransport() { close(); }

core::Result<std::unique_ptr<UsbByteTransport>>
UsbByteTransport::over_endpoint(std::unique_ptr<ByteEndpoint> endpoint) {
    if (!endpoint) {
        return Error{ErrorCode::invalid_argument};
    }
    auto opened = endpoint->open();
    if (!opened) {
        return opened.error();
    }
    return std::unique_ptr<UsbByteTransport>(
        new UsbByteTransport(std::make_unique<Impl>(std::move(endpoint))));
}

core::Result<void> UsbByteTransport::open(core::CancellationToken cancellation) {
    if (!impl_ || impl_->io.closed()) {
        return Error{ErrorCode::transport_closed};
    }
    token_ = std::move(cancellation);
    return {};
}

core::Result<void> UsbByteTransport::send(std::span<const std::byte> frame) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    auto sent = impl_->io.send_frame(frame, token_);
    if (!sent) {
        close();
    }
    return sent;
}

core::Result<std::vector<std::byte>> UsbByteTransport::receive() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    auto got = impl_->io.receive_frame(token_);
    if (!got) {
        close();
    }
    return got;
}

void UsbByteTransport::close() noexcept {
    if (impl_) {
        impl_->io.close();
        impl_->endpoint->close();
    }
}

} // namespace aa::transport
