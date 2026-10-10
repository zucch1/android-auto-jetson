// SPDX-License-Identifier: GPL-3.0-or-later
// In-memory USB byte-endpoint loopback (task 16): a bounded byte queue joining
// two endpoints, so the USB framing path is exercised without hardware. This is
// the seam task 28 replaces with a real AOA bulk endpoint.

#include <aa/transport/ByteEndpoint.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <utility>

namespace aa::transport {
namespace {

inline constexpr std::size_t kLoopbackBoundBytes = std::size_t{1024} * 1024u;

// One-directional bounded byte queue shared by a writer and a reader endpoint.
// condition_variable_any so the reader can wait on a stop token atomically
// (no lost-wake between the predicate check and the wait).
struct Channel final {
    std::deque<std::byte> queue{};
    std::mutex mutex{};
    std::condition_variable_any cv{};
    bool writer_closed{false};
    bool reader_closed{false};
};

class LoopbackEndpoint final : public ByteEndpoint {
public:
    LoopbackEndpoint(std::shared_ptr<Channel> in, std::shared_ptr<Channel> out)
        : in_(std::move(in)), out_(std::move(out)) {}

    ~LoopbackEndpoint() override { close(); }

    core::Result<void> open() override {
        std::lock_guard lock(in_->mutex);
        if (in_->reader_closed) {
            return Error{ErrorCode::transport_closed};
        }
        return {};
    }

    core::Result<std::size_t> read(std::span<std::byte> buffer,
                                   const core::CancellationToken& token) override {
        std::unique_lock lock(in_->mutex);
        // Stop-token-aware wait: the wakeup is registered atomically with the
        // wait, so a stop that fires between the predicate check and the block
        // cannot be lost (the lost-wake race a plain stop_callback leaves open).
        const bool ready = in_->cv.wait(lock, token, [this] {
            return !in_->queue.empty() || in_->writer_closed || in_->reader_closed;
        });
        if (!ready || token.stop_requested()) {
            return Error{ErrorCode::cancelled};
        }
        if (in_->reader_closed || in_->queue.empty()) {
            return std::size_t{0};
        }
        const std::size_t count = std::min(buffer.size(), in_->queue.size());
        for (std::size_t i = 0; i < count; ++i) {
            buffer[i] = in_->queue.front();
            in_->queue.pop_front();
        }
        return count;
    }

    core::Result<std::size_t> write(std::span<const std::byte> data,
                                  const core::CancellationToken& token) override {
        std::lock_guard lock(out_->mutex);
        if (token.stop_requested()) {
            return Error{ErrorCode::cancelled};
        }
        if (out_->writer_closed || out_->reader_closed) {
            return Error{ErrorCode::transport_closed};
        }
        if (data.size() > kLoopbackBoundBytes - out_->queue.size()) {
            return Error{ErrorCode::transport_queue_full};
        }
        out_->queue.insert(out_->queue.end(), data.begin(), data.end());
        out_->cv.notify_all();
        return data.size();
    }

    void close() noexcept override {
        {
            // Set the shared state and notify under the reader's mutex so a
            // reader between its predicate check and its wait cannot miss it.
            std::lock_guard lock(in_->mutex);
            in_->reader_closed = true;
            in_->queue.clear();
            in_->cv.notify_all();
        }
        {
            std::lock_guard lock(out_->mutex);
            out_->writer_closed = true;
            out_->cv.notify_all();
        }
    }

private:
    std::shared_ptr<Channel> in_;
    std::shared_ptr<Channel> out_;
};

} // namespace

core::Result<LoopbackPair> make_loopback() {
    auto forward = std::make_shared<Channel>();
    auto reverse = std::make_shared<Channel>();
    LoopbackPair pair;
    // a writes to `forward` and reads from `reverse`; b is the mirror.
    pair.a = std::make_unique<LoopbackEndpoint>(reverse, forward);
    pair.b = std::make_unique<LoopbackEndpoint>(forward, reverse);
    return pair;
}

} // namespace aa::transport
