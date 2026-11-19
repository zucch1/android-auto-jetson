// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/AawFrame.hpp>
#include <aa/helper/ApSnapshot.hpp>
#include <chrono>
#include <memory>

namespace aa::helper {
// RFCOMM socket ownership (#161). BlueZ keeps its own reference to every Profile1
// NewConnection socket, so close() here never sends DISC: only shutdown(2), which acts on
// the shared socket, frees the phone's Android Auto channel (DISC on that DLC only; HFP,
// the RFCOMM session and the ACL stay up). Bootstrap owns `owned_fd` from construction and
// releases it exactly once:
// - abort (deny(), the destructor, the confirmation deadline): shutdown(SHUT_RDWR), then close;
// - confirm() after a completed exchange: close only (completed RFCOMM teardown is not link loss).
// After completion the socket is kept, unsent and unread, until the receiver confirms the
// hand-off (its TCP session authenticated) or it is aborted, so a hand-off that fails after
// the last RFCOMM message still frees the phone. SocketExpiry holds a separate dup that it
// closes only after joining its thread, so no shutdown ever reaches a reused fd number.
class Bootstrap final {
public:
    using Time = std::chrono::steady_clock::time_point;
    static constexpr auto kBudget = std::chrono::seconds(10);
    // Backstop for a completed exchange the receiver never confirms or retires.
    static constexpr auto kConfirmBudget = std::chrono::seconds(30);
    Bootstrap(int owned_fd, Time accepted, std::function<Time()> now = std::chrono::steady_clock::now);
    ~Bootstrap();
    Bootstrap(const Bootstrap&) = delete;
    Bootstrap& operator=(const Bootstrap&) = delete;
    bool authorize(const ApSnapshot&, bool listener_ready, Time now);
    void tick(Time now, const SnapshotProvider&, const SnapshotFence&, bool single_step = false);
    void deny(HandoffFailure);
    // The receiver confirmed a completed hand-off: release the socket without shutdown.
    void confirm();
    HandoffFailure failure() const { return failure_; }
    bool complete() const { return stage_ == Stage::complete; }
    // The RFCOMM exchange is still running (the socket is read and written).
    bool live() const { return fd_ >= 0 && stage_ != Stage::complete; }
    // Completed, its socket held until confirm() or an abort.
    bool awaiting_confirmation() const { return fd_ >= 0 && stage_ == Stage::complete; }
    bool expire();
    SnapshotDeadline deadline() const { return complete() ? SnapshotDeadline{} : deadline_; }
private:
    enum class Stage { authorization, info, start, connect, finishing, complete, failed };
    bool queue(std::uint16_t id, const std::string& payload);
    bool flush(const SnapshotFence&);
    bool verify(const SnapshotProvider&);
    void record(const AawRecord&);
    bool drain(const SnapshotProvider&, const SnapshotFence&, bool single_step);
    void release(bool abort) noexcept;
    struct SocketExpiry;
    int fd_;
    Time confirm_until_{Time::max()};
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
