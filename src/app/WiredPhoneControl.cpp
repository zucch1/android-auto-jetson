// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WiredPhoneControl.hpp>
#include <aa/app/WiredSession.hpp>
#include <aa/trust/Store.hpp>

#include <cassert>
#include <utility>

namespace aa::app {

WiredPhoneControl::WiredPhoneControl(trust::ApprovedPhoneStore& store, WiredSession& session)
    : store_(store), session_(session) {}

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

void WiredPhoneControl::checkpoint() {
    assert_owner();
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
    auto result = store_.forget_phone(request->phone);
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

std::optional<helper::RevocationV1> WiredPhoneControl::take_revocation_v1() {
    const std::lock_guard lock{mutex_};
    auto result = std::move(revocation_);
    revocation_.reset();
    return result;
}

bool WiredPhoneControl::finish() {
    assert_owner();
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
    (void)finish();
    if (request) {
        complete(std::move(request), Error{ErrorCode::cancelled});
    }
}

} // namespace aa::app
