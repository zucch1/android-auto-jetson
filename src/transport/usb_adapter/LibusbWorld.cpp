// SPDX-License-Identifier: GPL-3.0-or-later
// libusb-backed AOA link factory (task 28). Device evidence comes from the
// libusb ABI only (port path via libusb_get_port_numbers, which matches the
// kernel's sysfs port name); no sysfs or external header is involved. See
// LibusbWorld.hpp and the error mapping in aa/transport/UsbAoa.hpp.

#include "LibusbWorld.hpp"

#include "LibusbAbi.hpp"
#include "UsbAoaSwitch.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aa::transport {
namespace usb_adapter {
namespace {

inline constexpr std::uint16_t kGoogleVendorId = 0x18D1;
inline constexpr std::uint16_t kAccessoryProductId = 0x2D00;
inline constexpr std::uint16_t kAccessoryAudioProductId = 0x2D01;
inline constexpr int kStringFetchLength = 255;
inline constexpr auto kReenumPollInterval = std::chrono::milliseconds(100);

// RAII libusb context shared by one factory instance.
class LibusbSession final {
  public:
    LibusbSession() = default;
    LibusbSession(const LibusbSession &) = delete;
    LibusbSession &operator=(const LibusbSession &) = delete;

    [[nodiscard]] core::Result<void> start() {
        auto loaded = LibusbApi::load();
        if (!loaded) {
            return loaded.error();
        }
        api_ = std::move(loaded.value());
        const int rc = api_.init(&ctx_);
        if (rc != kLibusbSuccess) {
            return map_libusb_error(rc);
        }
        return {};
    }

    ~LibusbSession() {
        if (api_.valid() && ctx_ != nullptr) {
            api_.exit(ctx_);
        }
    }

    [[nodiscard]] const LibusbApi &api() const noexcept { return api_; }
    [[nodiscard]] LibusbContext *context() const noexcept { return ctx_; }

  private:
    LibusbApi api_{};
    LibusbContext *ctx_{nullptr};
};

struct Candidate final {
    UsbDeviceSnapshot snapshot{};
    std::string port_path{};
    LibusbDevice *device{nullptr};
};

[[nodiscard]] std::string port_path_of(const LibusbApi &api, LibusbDevice *device) {
    std::uint8_t ports[7] = {};
    const int count = api.get_port_numbers(device, ports, 7);
    if (count <= 0) {
        return {};
    }
    std::string path = std::to_string(static_cast<int>(api.get_bus_number(device)));
    path.push_back('-');
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            path.push_back('.');
        }
        path += std::to_string(static_cast<int>(ports[i]));
    }
    return path;
}

// Enumerate and keep the descriptors + port paths; the device pointers are
// valid until the caller frees the list.
class Enumeration final {
  public:
    [[nodiscard]] core::Result<void> run(const LibusbApi &api, LibusbContext *ctx) {
        api_ = &api;
        const std::ptrdiff_t count = api.get_device_list(ctx, &list_);
        if (count < 0) {
            list_ = nullptr;
            return map_libusb_error(static_cast<int>(count));
        }
        candidates_.reserve(static_cast<std::size_t>(count));
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            LibusbDeviceDescriptor descriptor{};
            if (api.get_device_descriptor(list_[i], &descriptor) != kLibusbSuccess) {
                continue;
            }
            Candidate candidate;
            candidate.device = list_[i];
            candidate.snapshot.bus = static_cast<int>(api.get_bus_number(list_[i]));
            candidate.snapshot.address = static_cast<int>(api.get_device_address(list_[i]));
            candidate.snapshot.vendor_id = descriptor.idVendor;
            candidate.snapshot.product_id = descriptor.idProduct;
            candidate.port_path = port_path_of(api, list_[i]);
            candidates_.push_back(std::move(candidate));
        }
        return {};
    }

    ~Enumeration() {
        if (api_ != nullptr && list_ != nullptr) {
            api_->free_device_list(list_, 1);
        }
    }

    Enumeration() = default;
    Enumeration(const Enumeration &) = delete;
    Enumeration &operator=(const Enumeration &) = delete;

    [[nodiscard]] const std::vector<Candidate> &candidates() const noexcept { return candidates_; }

  private:
    const LibusbApi *api_{nullptr};
    LibusbDevice **list_{nullptr};
    std::vector<Candidate> candidates_{};
};

[[nodiscard]] bool matches(const UsbDeviceSelector &selector, const Candidate &candidate) {
    if (candidate.port_path != selector.port_context) {
        return false;
    }
    if (selector.bus >= 0 && candidate.snapshot.bus != selector.bus) {
        return false;
    }
    if (selector.address >= 0 && candidate.snapshot.address != selector.address) {
        return false;
    }
    if (selector.vendor_id != 0 && candidate.snapshot.vendor_id != selector.vendor_id) {
        return false;
    }
    if (selector.product_id != 0 && candidate.snapshot.product_id != selector.product_id) {
        return false;
    }
    return true;
}

[[nodiscard]] std::string fetch_string(const LibusbApi &api, LibusbDeviceHandle *handle,
                                       std::uint8_t index) {
    if (index == 0) {
        return {};
    }
    std::uint8_t buffer[kStringFetchLength] = {};
    const int length = api.get_string_descriptor_ascii(handle, index, buffer, kStringFetchLength);
    if (length <= 0) {
        // A device may refuse string fetches; the identity then carries less
        // material and admission decides. Never a crash, never an overflow.
        return {};
    }
    return std::string(reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(length));
}

// Open, fetch descriptor strings and claim accessory interface 0, detaching a
// kernel driver first when one is bound.
[[nodiscard]] core::Result<LibusbHandle> open_and_claim(const LibusbApi &api, LibusbDevice *device,
                                                        UsbDeviceSnapshot &snapshot) {
    LibusbDeviceHandle *raw = nullptr;
    const int rc = api.open(device, &raw);
    if (rc != kLibusbSuccess) {
        return map_libusb_error(rc);
    }
    LibusbHandle handle(&api, raw);
    LibusbDeviceDescriptor descriptor{};
    if (api.get_device_descriptor(device, &descriptor) == kLibusbSuccess) {
        snapshot.manufacturer = fetch_string(api, handle.get(), descriptor.iManufacturer);
        snapshot.product = fetch_string(api, handle.get(), descriptor.iProduct);
        snapshot.serial = fetch_string(api, handle.get(), descriptor.iSerialNumber);
    }
    const int active = api.kernel_driver_active(handle.get(), kAccessoryInterface);
    if (active > 0) {
        const int detached = api.detach_kernel_driver(handle.get(), kAccessoryInterface);
        if (detached != kLibusbSuccess && detached != kLibusbErrorNotSupported) {
            return map_libusb_error(detached);
        }
    }
    const int claimed = api.claim_interface(handle.get(), kAccessoryInterface);
    if (claimed != kLibusbSuccess) {
        return map_libusb_error(claimed);
    }
    return handle;
}

class LibusbControlChannel final : public AoaControlChannel {
  public:
    LibusbControlChannel(const LibusbApi &api, LibusbHandle handle) noexcept
        : api_(api), handle_(std::move(handle)) {}

    core::Result<std::vector<std::byte>> control_in(std::uint8_t request, std::uint16_t value,
                                                    std::uint16_t index,
                                                    std::uint16_t length) override {
        std::vector<std::byte> buffer(length);
        const int transferred = api_.control_transfer(
            handle_.get(), kAoaRequestIn, request, value, index,
            reinterpret_cast<std::uint8_t *>(buffer.data()), length, kAoaControlTimeoutMs);
        if (transferred < 0) {
            return map_libusb_error(transferred);
        }
        buffer.resize(static_cast<std::size_t>(transferred));
        return buffer;
    }

    core::Result<std::size_t> control_out(std::uint8_t request, std::uint16_t value,
                                          std::uint16_t index,
                                          std::span<const std::byte> payload) override {
        const auto *data = reinterpret_cast<const std::uint8_t *>(payload.data());
        const int transferred = api_.control_transfer(
            handle_.get(), kAoaRequestOut, request, value, index, const_cast<std::uint8_t *>(data),
            static_cast<std::uint16_t>(payload.size()), kAoaControlTimeoutMs);
        if (transferred < 0) {
            return map_libusb_error(transferred);
        }
        return static_cast<std::size_t>(transferred);
    }

  private:
    const LibusbApi &api_;
    LibusbHandle handle_;
};

class LibusbBulkPipe final : public UsbBulkPipe {
  public:
    LibusbBulkPipe(const LibusbApi &api, LibusbHandle handle) noexcept
        : api_(api), handle_(std::move(handle)) {}

    core::Result<std::size_t> read(std::span<std::byte> buffer, unsigned timeout_ms) override {
        return transfer(kAccessoryBulkIn, buffer, timeout_ms);
    }

    core::Result<std::size_t> write(std::span<const std::byte> data, unsigned timeout_ms) override {
        return transfer(kAccessoryBulkOut,
                        std::span<std::byte>{const_cast<std::byte *>(data.data()), data.size()},
                        timeout_ms);
    }

    void close() noexcept override { handle_.close(); }

  private:
    core::Result<std::size_t> transfer(std::uint8_t endpoint, std::span<std::byte> buffer,
                                       unsigned timeout_ms) {
        if (!handle_) {
            return Error{ErrorCode::transport_closed};
        }
        if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Error{ErrorCode::invalid_argument};
        }
        int transferred = 0;
        const int rc = api_.bulk_transfer(
            handle_.get(), endpoint, reinterpret_cast<std::uint8_t *>(buffer.data()),
            static_cast<int>(buffer.size()), &transferred, timeout_ms);
        if (rc == kLibusbSuccess) {
            return static_cast<std::size_t>(transferred);
        }
        if (rc == kLibusbErrorTimeout) {
            // A bounded wait with no (or partial) data is not an endpoint
            // failure: report what moved and let the caller re-poll/cancel.
            return static_cast<std::size_t>(std::max(transferred, 0));
        }
        return map_libusb_error(rc);
    }

    const LibusbApi &api_;
    LibusbHandle handle_;
};

class LibusbLinkFactory final : public AoaLinkFactory {
  public:
    core::Result<void> start() {
        auto started = session_.start();
        live_ = started.has_value();
        return started;
    }

    core::Result<void> switch_to_accessory(const UsbDeviceSelector &selector,
                                           const AoaAccessoryInfo &info,
                                           UsbDeviceSnapshot &pre_switch,
                                           AoaSwitchReport &switch_report) override {
        if (!live_) {
            return Error{ErrorCode::transport_unavailable};
        }
        auto validated = validate_selector(selector);
        if (!validated) {
            return validated.error();
        }
        Enumeration enumeration;
        auto enumerated = enumeration.run(session_.api(), session_.context());
        if (!enumerated) {
            return enumerated.error();
        }
        const Candidate *found = nullptr;
        for (const auto &candidate : enumeration.candidates()) {
            if (matches(selector, candidate)) {
                found = &candidate;
                break;
            }
        }
        if (found == nullptr) {
            return Error{ErrorCode::transport_unavailable};
        }
        pre_switch = found->snapshot;
        LibusbDevice *device = found->device;
        auto handle = open_and_claim(session_.api(), device, pre_switch);
        if (!handle) {
            return handle.error();
        }
        LibusbControlChannel channel(session_.api(), std::move(handle.value()));
        return run_aoa_switch(channel, info, switch_report);
    }

    core::Result<std::unique_ptr<UsbBulkPipe>>
    claim_accessory(const UsbDeviceSelector &selector, UsbDeviceSnapshot &post_switch) override {
        if (!live_) {
            return Error{ErrorCode::transport_unavailable};
        }
        auto validated = validate_selector(selector);
        if (!validated) {
            return validated.error();
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kAccessoryWaitMs.count);
        for (;;) {
            Enumeration enumeration;
            auto enumerated = enumeration.run(session_.api(), session_.context());
            if (!enumerated) {
                return enumerated.error();
            }
            const Candidate *found = nullptr;
            for (const auto &candidate : enumeration.candidates()) {
                const bool accessory = candidate.snapshot.vendor_id == kGoogleVendorId &&
                                       (candidate.snapshot.product_id == kAccessoryProductId ||
                                        candidate.snapshot.product_id == kAccessoryAudioProductId);
                if (accessory && candidate.port_path == selector.port_context) {
                    found = &candidate;
                    break;
                }
            }
            if (found != nullptr) {
                post_switch = found->snapshot;
                LibusbDevice *device = found->device;
                auto handle = open_and_claim(session_.api(), device, post_switch);
                if (!handle) {
                    return handle.error();
                }
                return std::unique_ptr<UsbBulkPipe>(
                    std::make_unique<LibusbBulkPipe>(session_.api(), std::move(handle.value())));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return Error{ErrorCode::timeout};
            }
            std::this_thread::sleep_for(kReenumPollInterval);
        }
    }

  private:
    LibusbSession session_{};
    bool live_{false};
};

} // namespace

core::Result<std::unique_ptr<AoaLinkFactory>> make_libusb_link_factory() {
    auto factory = std::make_unique<LibusbLinkFactory>();
    auto started = factory->start();
    if (!started) {
        return started.error();
    }
    return std::unique_ptr<AoaLinkFactory>(std::move(factory));
}

core::Result<void> validate_selector(const UsbDeviceSelector &selector) {
    const std::string &port = selector.port_context;
    if (port.empty() || port.size() > 64) {
        return Error{ErrorCode::invalid_argument};
    }
    std::size_t i = 0;
    auto digits = [&port, &i] {
        std::size_t count = 0;
        while (i < port.size() && port[i] >= '0' && port[i] <= '9') {
            ++i;
            ++count;
        }
        return count;
    };
    if (digits() == 0) {
        return Error{ErrorCode::invalid_argument};
    }
    if (i >= port.size() || port[i] != '-') {
        return Error{ErrorCode::invalid_argument};
    }
    ++i;
    if (digits() == 0) {
        return Error{ErrorCode::invalid_argument};
    }
    while (i < port.size()) {
        if (port[i] != '.') {
            return Error{ErrorCode::invalid_argument};
        }
        ++i;
        if (digits() == 0) {
            return Error{ErrorCode::invalid_argument};
        }
    }
    return {};
}

} // namespace usb_adapter

core::Result<UsbDeviceSnapshot> observe_usb_device(const UsbDeviceSelector &selector) {
    using namespace usb_adapter;
    auto validated = validate_selector(selector);
    if (!validated) {
        return validated.error();
    }
    LibusbSession session;
    auto started = session.start();
    if (!started) {
        return started.error();
    }
    Enumeration enumeration;
    auto enumerated = enumeration.run(session.api(), session.context());
    if (!enumerated) {
        return enumerated.error();
    }
    for (const auto &candidate : enumeration.candidates()) {
        if (matches(selector, candidate)) {
            return candidate.snapshot;
        }
    }
    return Error{ErrorCode::transport_unavailable};
}

core::Result<void> reset_port_device(const UsbDeviceSelector &selector) {
    using namespace usb_adapter;
    auto validated = validate_selector(selector);
    if (!validated) {
        return validated.error();
    }
    LibusbSession session;
    auto started = session.start();
    if (!started) {
        return started.error();
    }
    Enumeration enumeration;
    auto enumerated = enumeration.run(session.api(), session.context());
    if (!enumerated) {
        return enumerated.error();
    }
    const Candidate *found = nullptr;
    for (const auto &candidate : enumeration.candidates()) {
        if (candidate.port_path == selector.port_context) {
            found = &candidate;
            break;
        }
    }
    if (found == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    LibusbDeviceHandle *raw = nullptr;
    const int rc = session.api().open(found->device, &raw);
    if (rc != kLibusbSuccess) {
        return map_libusb_error(rc);
    }
    LibusbHandle handle(&session.api(), raw);
    const int reset = session.api().reset_device(handle.get());
    if (reset != kLibusbSuccess) {
        return map_libusb_error(reset);
    }
    return {};
}

} // namespace aa::transport
