// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>
#include <aa/transport/ByteEndpoint.hpp>
#include <aa/transport/Transport.hpp>

#include <memory>
#include <span>
#include <vector>

namespace aa::transport {

// USB byte-endpoint transport (task 16): moves complete AAP frames over any
// ByteEndpoint by splitting and rejoining the byte stream with the shared frame
// codec. This is the seam task 28 fills with a real wired USB/AOA bulk endpoint;
// it carries no libusb or hardware dependency here. Framing, bounds and typed
// malformed-input handling are identical to the TCP transport because both are
// backed by the same bounded frame stream.
class UsbByteTransport final : public Transport {
public:
    // Bind to a byte endpoint. The transport owns the endpoint for its
    // lifetime (a USB bulk endpoint in task 28, or the in-memory loopback here).
    [[nodiscard]] static core::Result<std::unique_ptr<UsbByteTransport>>
    over_endpoint(std::unique_ptr<ByteEndpoint> endpoint);

    ~UsbByteTransport() override;

    [[nodiscard]] Kind kind() const noexcept override { return Kind::usb_aoa; }

    core::Result<void> open(core::CancellationToken cancellation) override;
    core::Result<void> send(std::span<const std::byte> frame) override;
    [[nodiscard]] core::Result<std::vector<std::byte>> receive() override;
    void close() noexcept override;

private:
    struct Impl;
    explicit UsbByteTransport(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
    core::CancellationToken token_{};
};

} // namespace aa::transport
