// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WiredPhoneControl.hpp>
#include <aa/app/WiredSession.hpp>
#include <aa/trust/Store.hpp>

#include <cassert>
#include <utility>

namespace aa::app {

WiredPhoneControl::WiredPhoneControl(trust::ApprovedPhoneStore& store, WiredSession& session,
                                     Persist persist)
    : store_(store), session_(session), persist_(std::move(persist)) {}

WiredPhoneControl::~WiredPhoneControl() {
    // An owner that never reached finish(): never block destruction on disk.
    if (job_) {
        abandon_job();
    }
}

void WiredPhoneControl::bind_owner() {
    const std::lock_guard lock(mutex_);
    assert(owner_ == std::thread::id{});
    owner_ = std::this_thread::get_id();
}

void WiredPhoneControl::assert_owner() {
    const std::lock_guard lock(mutex_);
    assert(owner_ == std::this_thread::get_id());
}

void WiredPhoneControl::bind_phone(const trust::PhoneIdentity& phone) {
    assert_owner();
    phone_ = phone;
}

core::Result<void> WiredPhoneControl::forget(core::PhoneId phone) {
    auto request = std::make_unique<Request>();
    request->phone = phone;
    auto reply = request->reply.get_future();
    {
        const std::lock_guard lock(mutex_);
        if (!accepting_) {
            return Error{ErrorCode::cancelled};
        }
        if (occupied_) {
            return Error{ErrorCode::ipc_queue_full};
        }
        assert(owner_ != std::this_thread::get_id());
        occupied_ = true;
        queued_ = std::move(request);
    }
    return reply.get();
}

void WiredPhoneControl::complete(std::unique_ptr<Request> request, core::Result<void> result) {
    const std::lock_guard lock(mutex_);
    occupied_ = false;
    request->reply.set_value(result);
}

bool WiredPhoneControl::job_done(std::chrono::milliseconds wait) {
    std::unique_lock lock(job_->mutex);
    return job_->done_cv.wait_for(lock, wait, [&] { return job_->done; });
}

void WiredPhoneControl::checkpoint() {
    assert_owner();
    if (job_) {
        // A durable write is in flight: never wait for it on the session thread.
        if (!job_done(std::chrono::milliseconds{0})) {
            return;
        }
        settle();
        return;
    }
    if (pending_) {
        return; // the active phone was revoked; finish() completes it
    }
    std::unique_ptr<Request> request;
    {
        const std::lock_guard lock(mutex_);
        request = std::move(queued_);
    }
    if (!request) {
        return;
    }
    std::optional<helper::RevocationV1> snapshot;
    for (const auto& entry : store_.phones()) {
        if (entry.phone_id == request->phone) snapshot = helper::RevocationV1{entry.phone_id, entry.identity};
    }
    in_flight_ = std::move(request);
    in_flight_snapshot_ = std::move(snapshot);
    attempts_ = 0;
    start_job();
}

void WiredPhoneControl::start_job() {
    auto prepared = store_.prepare_forget(in_flight_->phone);
    if (!prepared.has_value()) {
        in_flight_snapshot_.reset();
        complete(std::move(in_flight_), prepared.error());
        return;
    }
    ++attempts_;
    prepared_ = std::move(prepared.value());
    job_ = std::make_shared<Job>();
    // The thread owns copies only (job state and the prepared document, with
    // its generation and the store's write sequencer), so an abandoned write
    // can outlive this object without touching it, and can never land over a
    // newer document.
    persistence_ = std::thread([job = job_, persist = persist_, prepared = *prepared_] {
        auto result = persist(prepared);
        const std::lock_guard lock(job->mutex);
        job->result = std::move(result);
        job->done = true;
        job->done_cv.notify_all();
    });
}

void WiredPhoneControl::settle() {
    persistence_.join();
    auto written = job_->result;
    job_.reset();
    if (store_.superseded(*prepared_) && attempts_ < kMaxAttempts) {
        // Another mutation landed while the write ran: the document is stale
        // and was either discarded or overwritten. Prepare and write again,
        // still off the owner (never a synchronous persist here).
        prepared_.reset();
        start_job();
        return;
    }
    auto request = std::move(in_flight_);
    auto snapshot = std::move(in_flight_snapshot_);
    auto result = store_.apply_forget(*prepared_, std::move(written));
    prepared_.reset();
    const bool committed = result.has_value() ||
                            result.error().code() == ErrorCode::trust_store_not_durable;
    {
        const std::lock_guard lock{mutex_};
        revocation_ = committed ? std::move(snapshot) : std::nullopt;
    }
    if (committed && phone_ && store_.lookup(*phone_) != trust::Decision::approved) {
        pending_result_ = result;
        pending_ = std::move(request);
        revoked_ = true;
        session_.request_stop();
        return;
    }
    complete(std::move(request), result);
}

void WiredPhoneControl::abandon_job() {
    // The write may still land later; memory stays unrevoked and the caller is
    // told the outcome is unknown (timeout), never success.
    persistence_.detach();
    job_.reset();
    prepared_.reset();
    in_flight_snapshot_.reset();
    if (in_flight_) {
        complete(std::move(in_flight_), Error{ErrorCode::timeout});
    }
}

std::optional<helper::RevocationV1> WiredPhoneControl::take_revocation_v1() {
    const std::lock_guard lock{mutex_};
    auto result = std::move(revocation_);
    revocation_.reset();
    return result;
}

void WiredPhoneControl::drain_persistence() {
    assert_owner();
    // Shutdown only: wait for an in-flight durable write up to a bound (each
    // superseded retry gets its own bound), then answer `timeout`.
    while (job_) {
        if (job_done(kShutdownPersistWait)) {
            settle();
        } else {
            abandon_job();
        }
    }
}

bool WiredPhoneControl::finish() {
    assert_owner();
    // Live path (polled under the session mutex): never waits on disk.
    if (job_ && job_done(std::chrono::milliseconds{0})) {
        settle();
    }
    if (pending_) {
        session_.tick();
        if (!session_.failure() && session_.state() != session::State::disconnected) {
            (void)session_.fail(Error{ErrorCode::session_unknown_phone});
        }
        complete(std::move(pending_), pending_result_);
    }
    return revoked_;
}

void WiredPhoneControl::close() {
    assert_owner();
    std::unique_ptr<Request> request;
    {
        const std::lock_guard lock(mutex_);
        accepting_ = false;
        request = std::move(queued_);
    }
    drain_persistence();
    (void)finish();
    if (request) {
        complete(std::move(request), Error{ErrorCode::cancelled});
    }
}

} // namespace aa::app
