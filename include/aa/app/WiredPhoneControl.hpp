// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/trust/Trust.hpp>
#include <aa/helper/Confirmation.hpp>
#include <aa/trust/Store.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace aa::app {
class WiredSession;

// ForgetPhone for the receiver's session. The store stays single-owner: the
// owner (session) thread prepares and applies every revocation, while the
// durable write (temp file, fsync, rename, directory fsync) runs on a
// per-request persistence thread that touches only the file (#90: a stalled
// disk must never stall the session's poll, A/V dispatch or audio credits).
// checkpoint() never waits on that write; finish() waits for it only up to
// kShutdownPersistWait, then answers the D-Bus caller with `timeout`.
class WiredPhoneControl final {
  public:
    using Persist = std::function<core::Result<void>(const trust::PreparedForget&)>;
    static constexpr std::chrono::milliseconds kShutdownPersistWait{2000};

    WiredPhoneControl(trust::ApprovedPhoneStore&, WiredSession&,
                      Persist persist = &trust::ApprovedPhoneStore::write_prepared);
    ~WiredPhoneControl();
    WiredPhoneControl(const WiredPhoneControl&) = delete;
    WiredPhoneControl& operator=(const WiredPhoneControl&) = delete;
    void bind_owner();
    void bind_phone(const trust::PhoneIdentity&);
    [[nodiscard]] core::Result<void> forget(core::PhoneId);
    std::optional<helper::RevocationV1> take_revocation_v1();
    void checkpoint();
    [[nodiscard]] bool finish();
    void close();

  private:
    struct Request {
        core::PhoneId phone;
        std::promise<core::Result<void>> reply;
    };
    struct Job {
        std::mutex mutex;
        std::condition_variable done_cv;
        bool done{false};
        core::Result<void> result{};
    };
    void assert_owner();
    void complete(std::unique_ptr<Request>, core::Result<void>);
    [[nodiscard]] bool job_done(std::chrono::milliseconds wait);
    void settle();
    void abandon_job();

    trust::ApprovedPhoneStore& store_;
    WiredSession& session_;
    std::mutex mutex_;
    std::thread::id owner_{};
    bool accepting_{true};
    bool occupied_{false};
    std::unique_ptr<Request> queued_;
    std::unique_ptr<Request> pending_;
    core::Result<void> pending_result_{};
    std::optional<trust::PhoneIdentity> phone_;
    bool revoked_{false};
    std::optional<helper::RevocationV1> revocation_;
    Persist persist_;
    std::shared_ptr<Job> job_;           // owner-only; shared with its persistence thread
    std::thread persistence_;
    std::unique_ptr<Request> in_flight_; // the request whose write is running
    std::optional<trust::PreparedForget> prepared_;
    std::optional<helper::RevocationV1> in_flight_snapshot_;
};
} // namespace aa::app
