// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/HandoffEligibility.hpp>
#include <aa/trust/Identity.hpp>
#include <atomic>
#include <future>

namespace aa::app {
using namespace std::chrono_literals;
struct HandoffEligibilityCoordinator::Impl {
    enum class Stage { retired, observing, preparing, authorizing, authorized };
    enum class Operation { observe, authorize, refresh, deny, withdraw };
    struct Job {
        Operation operation;
        std::uint64_t epoch;
        helper::Source source;
        std::uint64_t handle;
        bool credentials{true};
        Clock::time_point started{};
    };
    struct Reply { Job job; core::Result<HandoffObservation> result; };
    const trust::TrustStore& trust;
    HandoffOperations operations;
    Listener listener;
    std::atomic<std::uint64_t> epoch{1};
    Stage stage{Stage::retired};
    helper::Source source{};
    std::optional<core::ConsumerId> consumer;
    std::optional<trust::PhoneIdentity> phone;
    std::optional<helper::ApSnapshot> snapshot;
    std::uint64_t handle{}, retired_handle{};
    bool active{}, completed{}, confirming{};
    // 950ms evidence + <=25ms owner scheduling + <=25ms synchronous stop <=1s.
    Clock::time_point expires{Clock::time_point::max()}, establishment{}, grace{Clock::time_point::max()};
    std::optional<std::future<Reply>> pending;
    std::optional<Job> denial, withdrawal;

    Impl(const trust::TrustStore& store, HandoffOperations ops, Listener target)
        : trust(store), operations(std::move(ops)), listener(std::move(target)) {}

    bool live_consumer() const { return consumer && consumer->value == source.generation; }
    bool trusted() const { return phone && trust.lookup(*phone) == trust::Decision::approved; }
    bool valid() const {
        return stage == Stage::authorized && trusted() && Clock::now() < expires && Clock::now() < grace;
    }
    void launch(Job job) {
        job.started = Clock::now();
        pending.emplace(std::async(std::launch::async, [this, job]() -> Reply {
            if (job.operation != Operation::deny && job.epoch != epoch.load())
                return {job, Error{ErrorCode::cancelled}};
            if (job.operation == Operation::observe || job.operation == Operation::refresh)
                return {job, operations.validate({job.source, job.handle, job.credentials})};
            auto result = job.operation == Operation::withdraw ? operations.withdraw(job.source, job.handle)
                : operations.authorize(job.source, job.handle, job.operation == Operation::authorize);
            return {job, result ? core::Result<HandoffObservation>{HandoffObservation{}} :
                                 core::Result<HandoffObservation>{result.error()}};
        }));
    }
    void retire() {
        if (stage == Stage::retired) return;
        ++epoch;
        listener.stop();
        if (handle) {
            retired_handle = handle;
            denial = Job{Operation::deny, epoch.load(), source, handle};
        }
        stage = Stage::retired;
        withdrawal.reset(); completed = false; confirming = false;
        phone.reset(); snapshot.reset(); handle = 0;
    }
    void accept(Reply reply) {
        if (reply.job.epoch != epoch.load() || reply.job.operation == Operation::deny) return;
        if (!reply.result) {
            // A server-side RFCOMM peer may not have connected yet. Keep only
            // initial observation bounded by establishment; all later errors retire.
            if (reply.job.operation == Operation::observe && reply.result.error().code() == ErrorCode::helper_rejected)
                return;
            retire(); return;
        }
        const auto fresh_until = reply.job.started + 950ms;
        if (reply.job.operation == Operation::withdraw) {
            if (Clock::now() >= fresh_until || !trusted() || (!live_consumer() && !active)) { retire(); return; }
            expires = fresh_until; completed = true; confirming = false; return;
        }
        if (reply.job.operation == Operation::observe) {
            const auto& observed = reply.result.value();
            if (!observed.peer.handle || observed.peer.handle == retired_handle || !observed.snapshot) { retire(); return; }
            handle = observed.peer.handle;
            const auto key = trust::validated_key(trust::TransportIdentity{
                trust::WirelessIdentity{observed.peer.identity.address, observed.peer.identity.adapter}});
            if (!key) { retire(); return; }
            phone = trust::PhoneIdentity{key.value()};
            if (!trusted() || !live_consumer()) { retire(); return; }
            snapshot = observed.snapshot;
            stage = Stage::preparing;
        } else if (reply.job.operation == Operation::authorize) {
            if (Clock::now() >= fresh_until || !trusted() || !live_consumer()) { retire(); return; }
            expires = fresh_until; stage = Stage::authorized;
        } else {
            if (reply.job.credentials && !reply.result.value().snapshot) { retire(); return; }
            if (reply.result.value().snapshot) {
                if (reply.result.value().peer.handle != handle || !snapshot || *reply.result.value().snapshot != *snapshot)
                    { retire(); return; }
            } else {
                const auto& peer = reply.result.value().peer;
                const auto key = trust::validated_key(trust::TransportIdentity{
                    trust::WirelessIdentity{peer.identity.address, peer.identity.adapter}});
                if (peer.handle != handle || !key || !phone || key.value() != phone->key) { retire(); return; }
            }
            if (Clock::now() >= fresh_until || !trusted() || (!live_consumer() && !active)) { retire(); return; }
            expires = fresh_until;
            if (stage == Stage::preparing) {
                if (!live_consumer() || !listener.start(*snapshot)) { retire(); return; }
                stage = Stage::authorizing;
                launch({Operation::authorize, epoch.load(), source, handle});
            }
        }
    }
    void tick() {
        if (stage != Stage::retired && (Clock::now() >= expires || Clock::now() >= grace ||
            (stage != Stage::authorized && Clock::now() >= establishment) ||
            (phone && !trusted()) || (!live_consumer() && !active))) retire();
        if (pending && pending->wait_for(0ms) == std::future_status::ready) {
            auto reply = pending->get(); pending.reset(); accept(std::move(reply));
        }
        if (pending) return;
        if (denial) { auto job = *denial; denial.reset(); launch(job); return; }
        if (withdrawal) { auto job = *withdrawal; withdrawal.reset(); launch(job); return; }
        if (stage == Stage::observing) launch({Operation::observe, epoch.load(), source, 0});
        if (stage == Stage::preparing) launch({Operation::refresh, epoch.load(), source, handle});
        if (stage == Stage::authorized) {
            launch({Operation::refresh, epoch.load(), source, handle, live_consumer()});
        }
    }
};

HandoffEligibilityCoordinator::HandoffEligibilityCoordinator(const trust::TrustStore& trust,
    HandoffOperations operations, Listener listener)
    : impl_(std::make_unique<Impl>(trust, std::move(operations), std::move(listener))) {}
HandoffEligibilityCoordinator::~HandoffEligibilityCoordinator() {
    impl_->retire();
    if (impl_->pending) { impl_->pending->wait(); impl_->pending.reset(); }
    if (impl_->denial) {
        const auto job = *impl_->denial;
        (void)impl_->operations.authorize(job.source, job.handle, false);
    }
}
void HandoffEligibilityCoordinator::begin(helper::Source source) {
    impl_->retire();
    if (!source.generation || !impl_->consumer || impl_->consumer->value != source.generation) return;
    ++impl_->epoch;
    impl_->source = source;
    impl_->stage = Impl::Stage::observing;
    impl_->establishment = Clock::now() + 10s;
    impl_->expires = Clock::time_point::max();
    impl_->grace = Clock::time_point::max();
    impl_->active = false;
}
void HandoffEligibilityCoordinator::consumer(std::optional<core::ConsumerId> live) {
    impl_->consumer = live;
    if (impl_->stage == Impl::Stage::retired || impl_->live_consumer()) return;
    impl_->listener.stop();
    if (!live && impl_->active && impl_->completed) impl_->grace = std::min(impl_->grace, Clock::now() + 10s);
    else impl_->retire();
}
void HandoffEligibilityCoordinator::confirm_completion() {
    if (impl_->stage != Impl::Stage::authorized || impl_->completed || impl_->confirming) return;
    impl_->confirming = true;
    impl_->withdrawal = Impl::Job{Impl::Operation::withdraw, impl_->epoch.load(), impl_->source, impl_->handle};
}
bool HandoffEligibilityCoordinator::bootstrap_completed() const { return impl_->completed && impl_->valid(); }
void HandoffEligibilityCoordinator::tick(bool active) { impl_->active = active; impl_->tick(); }
void HandoffEligibilityCoordinator::retire() { impl_->retire(); }
bool HandoffEligibilityCoordinator::admitting() const { return impl_->valid() && impl_->live_consumer(); }
bool HandoffEligibilityCoordinator::valid_session() const { return impl_->valid(); }
std::optional<trust::PhoneIdentity> HandoffEligibilityCoordinator::phone() const { return impl_->phone; }
std::uint64_t HandoffEligibilityCoordinator::epoch() const { return impl_->epoch.load(); }
std::uint64_t HandoffEligibilityCoordinator::snapshot_generation() const {
    return impl_->snapshot ? impl_->snapshot->generation : 0;
}
}
