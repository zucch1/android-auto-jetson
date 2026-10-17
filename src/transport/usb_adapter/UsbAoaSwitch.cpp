// SPDX-License-Identifier: GPL-3.0-or-later
// AOA switch algorithm (task 28): faithful C++ port of
// tools/target/usb_aoa_libusb.py handshake(). See UsbAoaSwitch.hpp.

#include "UsbAoaSwitch.hpp"

#include <string_view>
#include <utility>

namespace aa::transport::usb_adapter {
namespace {

// Stable step names (probe-compatible; code constants, never device text).
inline constexpr std::string_view kStepGetProtocolLength = "get-protocol-length";
inline constexpr std::string_view kStepGetProtocolVersion = "get-protocol-version";
inline constexpr std::string_view kStepGetProtocolIo = "get-protocol-io";
inline constexpr std::string_view kStepStartShort = "start-short";
inline constexpr std::string_view kStepStartIo = "start-io";

[[nodiscard]] std::string send_string_step_name(std::uint16_t index) {
    switch (index) {
    case kAoaStringManufacturer:
        return "send-string-manufacturer";
    case kAoaStringModel:
        return "send-string-model";
    case kAoaStringDescription:
        return "send-string-description";
    case kAoaStringVersion:
        return "send-string-version";
    case kAoaStringUri:
        return "send-string-uri";
    case kAoaStringSerial:
        return "send-string-serial";
    default:
        return "send-string-unknown";
    }
}

[[nodiscard]] std::string send_string_io_step_name(std::uint16_t index) {
    return send_string_step_name(index) + "-io";
}

struct SendStringSpec final {
    std::uint16_t index;
    std::string text;
};

[[nodiscard]] core::Result<std::vector<SendStringSpec>>
send_string_specs(const AoaAccessoryInfo &info) {
    SendStringSpec specs[6] = {
        {kAoaStringManufacturer, info.manufacturer},
        {kAoaStringModel, info.model},
        {kAoaStringDescription, info.description},
        {kAoaStringVersion, info.version},
        {kAoaStringUri, info.uri},
        {kAoaStringSerial, info.serial},
    };
    std::vector<SendStringSpec> out;
    out.reserve(6);
    for (const auto &spec : specs) {
        const auto payload = aoa_string_payload(spec.text);
        if (!payload) {
            return payload.error();
        }
        out.push_back(SendStringSpec{spec.index, spec.text});
    }
    return out;
}

} // namespace

core::Result<std::vector<std::byte>> aoa_string_payload(const std::string &text) {
    // Receiver-side configuration text is bounded printable ASCII; the wire
    // form is the ASCII bytes plus one NUL (exactly what the probe sends).
    // The wire payload itself must fit the AOA SEND_STRING limit
    // (kAoaStringMaxWireBytes incl. NUL), which caps the text at 255 chars -
    // tighter than the identity field budget.
    if (text.size() > kAoaStringMaxChars) {
        return Error{ErrorCode::invalid_argument};
    }
    std::vector<std::byte> payload;
    payload.reserve(text.size() + 1);
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte > 0x7EU) {
            return Error{ErrorCode::invalid_argument};
        }
        payload.push_back(static_cast<std::byte>(byte));
    }
    payload.push_back(std::byte{0});
    return payload;
}

core::Result<void> run_aoa_switch(AoaControlChannel &channel, const AoaAccessoryInfo &info,
                                  AoaSwitchReport &report) {
    const auto specs = send_string_specs(info);
    if (!specs) {
        return specs.error();
    }
    report = AoaSwitchReport{};
    report.failed_at = std::string{kStepGetProtocolIo};

    // GET_PROTOCOL (51): vendor IN 0xC0, wValue/wIndex 0, exactly 2 bytes.
    auto protocol = channel.control_in(kAoaRequestGetProtocol, 0, 0, kAoaProtocolLength);
    report.requests = 1;
    if (!protocol) {
        return protocol.error();
    }
    if (protocol.value().size() != kAoaProtocolLength) {
        report.failed_at = std::string{kStepGetProtocolLength};
        return Error{ErrorCode::transport_malformed_frame};
    }
    report.protocol_version =
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(protocol.value()[0]) |
                                   (static_cast<std::uint16_t>(protocol.value()[1]) << 8U));
    if (report.protocol_version < 1) {
        report.failed_at = std::string{kStepGetProtocolVersion};
        return Error{ErrorCode::transport_unavailable};
    }

    // SEND_STRING (52) x6: vendor OUT 0x40, wValue 0, wIndex = string slot.
    for (const auto &spec : specs.value()) {
        const auto payload = aoa_string_payload(spec.text);
        if (!payload) {
            return payload.error();
        }
        const std::uint16_t index = spec.index;
        auto sent = channel.control_out(kAoaRequestSendString, 0, index, payload.value());
        ++report.requests;
        const std::string name = send_string_step_name(index);
        if (!sent) {
            report.failed_at = send_string_io_step_name(index);
            return sent.error();
        }
        report.steps.push_back(AoaStringStep{index, payload.value().size(), sent.value()});
        if (sent.value() != payload.value().size()) {
            report.failed_at = name;
            return Error{ErrorCode::transport_io};
        }
    }

    // START (53): vendor OUT 0x40, empty payload; must return 0 transferred.
    auto started = channel.control_out(kAoaRequestStart, 0, 0, {});
    ++report.requests;
    if (!started) {
        report.failed_at = std::string{kStepStartIo};
        return started.error();
    }
    if (started.value() != 0) {
        report.failed_at = std::string{kStepStartShort};
        return Error{ErrorCode::transport_io};
    }

    report.started = true;
    report.failed_at.clear();
    return {};
}

} // namespace aa::transport::usb_adapter
