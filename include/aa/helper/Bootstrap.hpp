// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/AawFrame.hpp>
#include <aa/helper/ApSnapshot.hpp>
#include <chrono>
#include <memory>

namespace aa::helper {
class Bootstrap final {
public:
    using Time = std::chrono::steady_clock::time_point;
    static constexpr auto kBudget = std::chrono::seconds(10);
    Bootstrap(int owned_fd, Time accepted, std::function<Time()> now = std::chrono::steady_clock::now);
    ~Bootstrap();
    Bootstrap(const Bootstrap&) = delete;
    Bootstrap& operator=(const Bootstrap&) = delete;
    bool authorize(const ApSnapshot&, bool listener_ready, Time now);
    void tick(Time now, const SnapshotProvider&, const SnapshotFence&, bool single_step = false);
    void deny(HandoffFailure);
    HandoffFailure failure() const { return failure_; }
    bool complete() const { return stage_ == Stage::complete; }
    bool live() const { return fd_ >= 0; }
    bool expire();
    SnapshotDeadline deadline() const { return complete() ? SnapshotDeadline{} : deadline_; }
private:
    enum class Stage { authorization, info, start, connect, finishing, complete, failed };
    bool queue(std::uint16_t id, const std::string& payload);
    bool flush(const SnapshotFence&);
    bool verify(const SnapshotProvider&);
    void record(const AawRecord&);
    bool drain(const SnapshotProvider&, const SnapshotFence&, bool single_step);
    struct SocketExpiry;
    int fd_;
    SnapshotDeadline deadline_;
    std::unique_ptr<SocketExpiry> expiry_;
    Stage stage_{Stage::authorization};
    HandoffFailure failure_{HandoffFailure::none};
    ApSnapshot snapshot_;
    AawDecoder decoder_;
    std::vector<std::uint8_t> output_;
    std::size_t sent_{};
    bool seen_info_{}, seen_start_{}, modern_{};
};
}
