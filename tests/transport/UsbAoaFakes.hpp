// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Scripted device fixtures for the task-28 USB/AOA unit tests. The fakes model
// the hardware seams (AOA control channel, accessory bulk pipe, link factory)
// so the real switch algorithm, connection pipeline and endpoint are exercised
// without hardware: device-supplied text, hotplug detach, timeouts and
// cancellation are all scriptable.

#include "../../src/transport/usb_adapter/UsbAoaDevice.hpp"
#include "../../src/transport/usb_adapter/UsbAoaSwitch.hpp"

#include <aa/core/Error.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace aa::test::usb_aoa {

namespace ua = aa::transport::usb_adapter;

// Scripted AOA control channel. Defaults reproduce the reference probe's
// contract: GET_PROTOCOL 2 bytes version 2, every SEND_STRING acked at full
// length, START returns 0. Each knob corrupts one contract point.
class FakeControlChannel final : public ua::AoaControlChannel {
  public:
    struct Call final {
        std::uint8_t request{};
        std::uint16_t value{};
        std::uint16_t index{};
        std::size_t length{};
    };

    std::size_t protocol_length{2};
    std::uint16_t protocol_version{2};
    std::size_t send_string_short_by{0};
    int start_transferred{0};
    bool fail_control_in{false};
    bool fail_control_out{false};
    Error fail_error{ErrorCode::transport_io};
    std::vector<Call> calls{};
    std::vector<std::byte> last_out_payload{};

    core::Result<std::vector<std::byte>> control_in(std::uint8_t request, std::uint16_t value,
                                                    std::uint16_t index,
                                                    std::uint16_t length) override {
        calls.push_back(Call{request, value, index, length});
        if (fail_control_in) {
            return fail_error;
        }
        std::vector<std::byte> reply(protocol_length, std::byte{0});
        if (protocol_length >= 2) {
            reply[0] = static_cast<std::byte>(protocol_version & 0xFFU);
            reply[1] = static_cast<std::byte>((protocol_version >> 8U) & 0xFFU);
        }
        return reply;
    }

    core::Result<std::size_t> control_out(std::uint8_t request, std::uint16_t value,
                                          std::uint16_t index,
                                          std::span<const std::byte> payload) override {
        calls.push_back(Call{request, value, index, payload.size()});
        last_out_payload.assign(payload.begin(), payload.end());
        if (fail_control_out) {
            return fail_error;
        }
        if (payload.empty()) {
            return static_cast<std::size_t>(start_transferred < 0 ? 0 : start_transferred);
        }
        if (payload.size() < send_string_short_by) {
            return std::size_t{0};
        }
        return payload.size() - send_string_short_by;
    }
};

// Scripted accessory bulk pipe. `push_in` plays the phone; `out()` captures what
// the receiver sent. `detach()` models hotplug: later transfers fail typed.
class FakeBulkPipe final : public ua::UsbBulkPipe {
  public:
    core::Result<std::size_t> read(std::span<std::byte> buffer, unsigned timeout_ms) override {
        ++reads_started;
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this] { return !in_.empty() || detached_ || closed_; });
        if (detached_ || closed_) {
            return Error{ErrorCode::transport_closed};
        }
        if (in_.empty()) {
            return std::size_t{0};
        }
        const std::size_t count = std::min(buffer.size(), in_.size());
        for (std::size_t i = 0; i < count; ++i) {
            buffer[i] = in_.front();
            in_.pop_front();
        }
        return count;
    }

    core::Result<std::size_t> write(std::span<const std::byte> data, unsigned timeout_ms) override {
        (void)timeout_ms;
        std::lock_guard lock(mutex_);
        if (detached_ || closed_) {
            return Error{ErrorCode::transport_closed};
        }
        if (data.empty()) {
            return std::size_t{0};
        }
        out_.insert(out_.end(), data.begin(), data.end());
        return data.size();
    }

    void close() noexcept override {
        std::lock_guard lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    void push_in(const std::vector<std::byte> &bytes) {
        std::lock_guard lock(mutex_);
        in_.insert(in_.end(), bytes.begin(), bytes.end());
        cv_.notify_all();
    }

    void detach() {
        std::lock_guard lock(mutex_);
        detached_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<std::byte> out() const {
        std::lock_guard lock(mutex_);
        return out_;
    }

    [[nodiscard]] std::size_t out_size() const {
        std::lock_guard lock(mutex_);
        return out_.size();
    }

    [[nodiscard]] int reads() const noexcept { return reads_started.load(); }

  private:
    mutable std::mutex mutex_{};
    std::condition_variable cv_{};
    std::deque<std::byte> in_{};
    std::vector<std::byte> out_{};
    bool detached_{false};
    bool closed_{false};
    std::atomic<int> reads_started{0};
};

// Device fixture around the connection pipeline: phone-mode and accessory-mode
// descriptor snapshots, a scripted control channel fed through the REAL switch
// algorithm, and a fresh bulk pipe per claim (fresh discovery semantics).
class FakeLinkFactory final : public ua::AoaLinkFactory {
  public:
    aa::transport::UsbDeviceSnapshot phone{
        1, 7, 0x04E8, 0x6860, "TEST-SERIAL-001", "SAMSUNG", "SAMSUNG_Android"};
    aa::transport::UsbDeviceSnapshot accessory{
        1, 8, 0x18D1, 0x2D00, "TEST-SERIAL-001", "SAMSUNG", "SAMSUNG_Android"};
    FakeControlChannel control{};
    bool fail_switch{false};
    Error switch_error{ErrorCode::transport_unavailable};
    bool fail_claim{false};
    Error claim_error{ErrorCode::timeout};
    int switch_calls{0};
    int claim_calls{0};
    // Non-owning observers: each claimed pipe is OWNED by the endpoint; tests
    // inspect it only while that endpoint is alive.
    std::vector<FakeBulkPipe *> pipes{};
    std::string captured_port{};

    core::Result<void> switch_to_accessory(const aa::transport::UsbDeviceSelector &selector,
                                           const aa::transport::AoaAccessoryInfo &info,
                                           aa::transport::UsbDeviceSnapshot &pre_switch,
                                           aa::transport::AoaSwitchReport &switch_report) override {
        ++switch_calls;
        captured_port = selector.port_context;
        if (selector.product_id != 0 && selector.product_id != phone.product_id) {
            return Error{ErrorCode::transport_unavailable};
        }
        if (fail_switch) {
            return switch_error;
        }
        pre_switch = phone;
        return ua::run_aoa_switch(control, info, switch_report);
    }

    core::Result<std::unique_ptr<ua::UsbBulkPipe>>
    claim_accessory(const aa::transport::UsbDeviceSelector &selector,
                    aa::transport::UsbDeviceSnapshot &post_switch) override {
        ++claim_calls;
        (void)selector;
        if (fail_claim) {
            return claim_error;
        }
        post_switch = accessory;
        auto pipe = std::make_unique<FakeBulkPipe>();
        pipes.push_back(pipe.get());
        return std::unique_ptr<ua::UsbBulkPipe>(std::move(pipe));
    }

    [[nodiscard]] FakeBulkPipe *pipe(std::size_t index = 0) const {
        return index < pipes.size() ? pipes[index] : nullptr;
    }
};

} // namespace aa::test::usb_aoa
