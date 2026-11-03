// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Scripted device fixtures for the task-28 USB/AOA unit tests. The fakes model
// the hardware seams (AOA control channel, accessory bulk pipe, link factory)
// so the real switch algorithm, connection pipeline and endpoint are exercised
// without hardware: device-supplied text, hotplug detach, timeouts and
// cancellation are all scriptable.
//
// LIFETIME (gate P1-1): every observation an assertion reads lives in a
// test-owned shared block (LinkState / PipeState) that OUTLIVES the fakes.
// The connection pipeline owns and may destroy its factory/pipe before
// returning; assertions after any API call read only the shared block, so no
// test can dereference freed fixture memory. Pipe/factory objects remain for
// DRIVING (push_in/detach/knobs) strictly while their owner is alive.

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
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace aa::test::usb_aoa {

namespace ua = aa::transport::usb_adapter;

// One scripted control-channel call record.
struct ControlCall final {
    std::uint8_t request{};
    std::uint16_t value{};
    std::uint16_t index{};
    std::size_t length{};
};

// Test-owned per-pipe observations. Mirrors exactly what an assertion needs
// after the endpoint (and with it the pipe) may have been destroyed.
struct PipeState final {
    mutable std::mutex mutex{};
    std::vector<std::byte> out{};
    std::atomic<int> reads_started{0};
    std::atomic<unsigned> last_read_timeout_ms{0};
    std::atomic<int> writes_started{0};
    bool detached{false};
    bool closed{false};

    [[nodiscard]] std::vector<std::byte> out_bytes() const {
        std::lock_guard lock(mutex);
        return out;
    }
    [[nodiscard]] std::size_t out_size() const {
        std::lock_guard lock(mutex);
        return out.size();
    }
};

// Test-owned link observations shared by the control channel and the factory.
struct LinkState final {
    mutable std::mutex mutex{};
    int switch_calls{0};
    int claim_calls{0};
    std::string captured_port{};
    std::vector<ControlCall> control_calls{};
    std::vector<std::byte> last_control_out{};
    std::vector<std::shared_ptr<PipeState>> pipes{};

    [[nodiscard]] int switches() const {
        std::lock_guard lock(mutex);
        return switch_calls;
    }
    [[nodiscard]] int claims() const {
        std::lock_guard lock(mutex);
        return claim_calls;
    }
    [[nodiscard]] std::string port() const {
        std::lock_guard lock(mutex);
        return captured_port;
    }
    [[nodiscard]] std::vector<ControlCall> calls() const {
        std::lock_guard lock(mutex);
        return control_calls;
    }
    [[nodiscard]] std::shared_ptr<PipeState> pipe_state(std::size_t index = 0) const {
        std::lock_guard lock(mutex);
        return index < pipes.size() ? pipes[index] : nullptr;
    }
};

// Scripted AOA control channel. Defaults reproduce the reference probe's
// contract: GET_PROTOCOL 2 bytes version 2, every SEND_STRING acked at full
// length, START returns 0. Each knob corrupts one contract point. Call records
// land in the shared LinkState.
class FakeControlChannel final : public ua::AoaControlChannel {
  public:
    explicit FakeControlChannel(std::shared_ptr<LinkState> state = std::make_shared<LinkState>())
        : state_(std::move(state)) {}

    std::size_t protocol_length{2};
    std::uint16_t protocol_version{2};
    std::size_t send_string_short_by{0};
    int start_transferred{0};
    bool fail_control_in{false};
    bool fail_control_out{false};
    Error fail_error{ErrorCode::transport_io};

    [[nodiscard]] std::shared_ptr<LinkState> state() const noexcept { return state_; }

    core::Result<std::vector<std::byte>> control_in(std::uint8_t request, std::uint16_t value,
                                                    std::uint16_t index,
                                                    std::uint16_t length) override {
        record(ControlCall{request, value, index, length});
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
        record(ControlCall{request, value, index, payload.size()});
        {
            std::lock_guard lock(state_->mutex);
            state_->last_control_out.assign(payload.begin(), payload.end());
        }
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

  private:
    void record(const ControlCall &call) {
        std::lock_guard lock(state_->mutex);
        state_->control_calls.push_back(call);
    }

    std::shared_ptr<LinkState> state_;
};

// Scripted accessory bulk pipe. `push_in` plays the phone; observed outbound
// bytes land in the shared PipeState. `detach()` models hotplug: later
// transfers fail typed. `zero_progress_writes` makes write() report a bounded
// timeout with no progress (gate P2-4); `zero_progress_reads` does the same
// for read() so its retry policy can be exercised; `block_writes` holds write()
// inside the pipe's wait (kernel futex) until release/detach/close or the
// caller's timeout, so cancellation can be proven IN FLIGHT on the OUT path.
class FakeBulkPipe final : public ua::UsbBulkPipe {
  public:
    explicit FakeBulkPipe(std::shared_ptr<PipeState> state = std::make_shared<PipeState>())
        : state_(std::move(state)) {}

    int zero_progress_writes{0};
    int zero_progress_reads{0};
    bool block_writes{false};
    std::size_t max_read_bytes{std::numeric_limits<std::size_t>::max()};
    std::size_t max_write_bytes{std::numeric_limits<std::size_t>::max()};
    std::size_t fail_write_after{std::numeric_limits<std::size_t>::max()};
    int stall_after_first_write{0};

    [[nodiscard]] std::shared_ptr<PipeState> state() const noexcept { return state_; }

    core::Result<std::size_t> read(std::span<std::byte> buffer, unsigned timeout_ms) override {
        ++state_->reads_started;
        state_->last_read_timeout_ms.store(timeout_ms);
        std::unique_lock lock(mutex_);
        if (detached_ || closed_) {
            return Error{ErrorCode::transport_closed};
        }
        if (zero_progress_reads > 0) {
            --zero_progress_reads;
            return std::size_t{0};
        }
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this] { return !in_.empty() || detached_ || closed_; });
        if (detached_ || closed_) {
            return Error{ErrorCode::transport_closed};
        }
        if (in_.empty()) {
            return std::size_t{0};
        }
        const std::size_t count = std::min({buffer.size(), in_.size(), max_read_bytes});
        for (std::size_t i = 0; i < count; ++i) {
            buffer[i] = in_.front();
            in_.pop_front();
        }
        return count;
    }

    core::Result<std::size_t> write(std::span<const std::byte> data, unsigned timeout_ms) override {
        if (state_->out_size() >= fail_write_after) {
            return Error{ErrorCode::transport_io};
        }
        if (zero_progress_writes > 0) {
            --zero_progress_writes;
            std::lock_guard lock(mutex_);
            if (detached_ || closed_) {
                return Error{ErrorCode::transport_closed};
            }
            return std::size_t{0};
        }
        std::unique_lock lock(mutex_);
        if (block_writes) {
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !block_writes || detached_ || closed_; });
            if (detached_ || closed_) {
                return Error{ErrorCode::transport_closed};
            }
            // Still held at the bounded timeout: no progress, as a stalled OUT.
            return std::size_t{0};
        }
        if (detached_ || closed_) {
            return Error{ErrorCode::transport_closed};
        }
        ++state_->writes_started;
        if (data.empty()) {
            return std::size_t{0};
        }
        {
            std::lock_guard observed(state_->mutex);
            const auto count = std::min(data.size(), max_write_bytes);
            state_->out.insert(state_->out.end(), data.begin(), data.begin() +
                                                                  static_cast<std::ptrdiff_t>(count));
        }
        zero_progress_writes = std::exchange(stall_after_first_write, 0);
        return std::min(data.size(), max_write_bytes);
    }

    void release_writes() {
        std::lock_guard lock(mutex_);
        block_writes = false;
        cv_.notify_all();
    }

    void close() noexcept override {
        std::lock_guard lock(mutex_);
        closed_ = true;
        {
            std::lock_guard observed(state_->mutex);
            state_->closed = true;
        }
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
        {
            std::lock_guard observed(state_->mutex);
            state_->detached = true;
        }
        cv_.notify_all();
    }

  private:
    std::shared_ptr<PipeState> state_;
    std::mutex mutex_{};
    std::condition_variable cv_{};
    std::deque<std::byte> in_{};
    bool detached_{false};
    bool closed_{false};
};

// Device fixture around the connection pipeline: phone-mode and accessory-mode
// descriptor snapshots, a scripted control channel fed through the REAL switch
// algorithm, and a fresh bulk pipe per claim (fresh discovery semantics). All
// observations land in the test-owned `state`.
class FakeLinkFactory final : public ua::AoaLinkFactory {
  public:
    explicit FakeLinkFactory(std::shared_ptr<LinkState> state = std::make_shared<LinkState>())
        : state_(std::move(state)), control_(state_) {}

    aa::transport::UsbDeviceSnapshot phone{
        1, 7, 0x04E8, 0x6860, "TEST-SERIAL-001", "SAMSUNG", "SAMSUNG_Android"};
    aa::transport::UsbDeviceSnapshot accessory{
        1, 8, 0x18D1, 0x2D00, "TEST-SERIAL-001", "SAMSUNG", "SAMSUNG_Android"};
    bool fail_switch{false};
    Error switch_error{ErrorCode::transport_unavailable};
    bool fail_claim{false};
    Error claim_error{ErrorCode::timeout};
    int zero_progress_writes{0};
    int zero_progress_reads{0};

    FakeControlChannel &control() noexcept { return control_; }
    [[nodiscard]] std::shared_ptr<LinkState> state() const noexcept { return state_; }

    // Driving access to the most recent pipe while its owner is still alive.
    [[nodiscard]] FakeBulkPipe *pipe(std::size_t index = 0) const {
        std::lock_guard lock(driving_mutex_);
        return index < driving_.size() ? driving_[index] : nullptr;
    }

    core::Result<void> switch_to_accessory(const aa::transport::UsbDeviceSelector &selector,
                                           const aa::transport::AoaAccessoryInfo &info,
                                           aa::transport::UsbDeviceSnapshot &pre_switch,
                                           aa::transport::AoaSwitchReport &switch_report) override {
        {
            std::lock_guard lock(state_->mutex);
            ++state_->switch_calls;
            state_->captured_port = selector.port_context;
        }
        if (selector.product_id != 0 && selector.product_id != phone.product_id) {
            return Error{ErrorCode::transport_unavailable};
        }
        if (fail_switch) {
            return switch_error;
        }
        pre_switch = phone;
        return ua::run_aoa_switch(control_, info, switch_report);
    }

    core::Result<std::unique_ptr<ua::UsbBulkPipe>>
    claim_accessory(const aa::transport::UsbDeviceSelector &selector,
                    aa::transport::UsbDeviceSnapshot &post_switch) override {
        (void)selector;
        {
            std::lock_guard lock(state_->mutex);
            ++state_->claim_calls;
        }
        if (fail_claim) {
            return claim_error;
        }
        post_switch = accessory;
        auto pipe_state = std::make_shared<PipeState>();
        {
            std::lock_guard lock(state_->mutex);
            state_->pipes.push_back(pipe_state);
        }
        auto pipe = std::make_unique<FakeBulkPipe>(pipe_state);
        pipe->zero_progress_writes = zero_progress_writes;
        pipe->zero_progress_reads = zero_progress_reads;
        {
            std::lock_guard lock(driving_mutex_);
            driving_.push_back(pipe.get());
        }
        return std::unique_ptr<ua::UsbBulkPipe>(std::move(pipe));
    }

  private:
    std::shared_ptr<LinkState> state_;
    FakeControlChannel control_;
    mutable std::mutex driving_mutex_{};
    std::vector<FakeBulkPipe *> driving_{};
};

} // namespace aa::test::usb_aoa
