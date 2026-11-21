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
    // From the first observation of `handle`: the bootstrap's own budget (#161).
    Clock::time_point bootstrap_until{};
    Timing timing;
    std::optional<std::future<Reply>> pending;
    std::optional<Job> denial, withdrawal;

    Impl(const trust::TrustStore& store, HandoffOperations ops, Listener target, Timing bounds)
        : trust(store), operations(std::move(ops)), listener(std::move(target)), timing(bounds) {}

    bool live_consumer() const { return consumer && consumer->value == source.generation; }
    bool trusted() const { return phone && trust.lookup(*phone) == trust::Decision::approved; }
    bool valid() const {
        return stage == Stage::authorized && trusted() && Clock::now() < expires && Clock::now() < grace;
    }
    bool in_progress() const {
        return stage == Stage::preparing || stage == Stage::authorizing || (stage == Stage::authorized && !completed);
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
    // A stale observation may still name a handle: PollBootstrapV1 bound the phone's live
    // bootstrap to this attempt's source before the attempt was retired. It is denied like any
    // retired handle and never adopted again; otherwise the helper keeps the bootstrap, its
    // binding and its lease into the next attempt (#169 review).
    void settle_reply(const Reply& reply) {
        if (reply.job.operation == Operation::deny) {
            if (reply.result) denial.reset();
            return;
        }
        if (reply.job.operation != Operation::observe || !reply.result || !reply.result.value().peer.handle) return;
        const auto bound = reply.result.value().peer.handle;
        retired_handle = bound;
        // The successful poll now owns the helper's single binding.
        denial = Job{Operation::deny, epoch.load(), reply.job.source, bound};
    }
    void accept(Reply reply) {
        if (reply.job.operation == Operation::deny || reply.job.epoch != epoch.load()) {
            settle_reply(reply);
            return;
        }
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
            // The helper accepted it before this reply, so its own deadline comes first.
            bootstrap_until = Clock::now() + timing.bootstrap;
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
    // Sends this coordinator's outstanding denial now, after any operation still in flight (whose
    // reply is stale once retired, but may name a handle to deny). Keep a failed denial for tick().
    void settle() {
        if (pending) {
            auto reply = pending->get();
            pending.reset();
            settle_reply(reply);
            if (reply.job.operation == Operation::deny) return;
        }
        if (denial && operations.authorize(denial->source, denial->handle, false)) denial.reset();
    }
    void tick() {
        // Establishment bounds only the wait for a bootstrap. An observed one runs to its own
        // budget, however late in the attempt the phone connected (#161).
        if (stage != Stage::retired && (Clock::now() >= expires || Clock::now() >= grace ||
            (stage == Stage::observing && Clock::now() >= establishment) ||
            ((stage == Stage::preparing || stage == Stage::authorizing) && Clock::now() >= bootstrap_until) ||
            (phone && !trusted()) || (!live_consumer() && !active))) retire();
        bool finished_denial = false;
        if (pending && pending->wait_for(0ms) == std::future_status::ready) {
            auto reply = pending->get();
            pending.reset();
            finished_denial = reply.job.operation == Operation::deny;
            accept(std::move(reply));
        }
        if (pending) return;
        // A gone handle can be rejected on every retry. Give a replacement attempt an
        // observation between retries, then let it proceed once it has its own handle.
        if (denial && (stage == Stage::retired || (stage == Stage::observing && !finished_denial))) {
            launch(*denial);
            return;
        }
        if (withdrawal) { auto job = *withdrawal; withdrawal.reset(); launch(job); return; }
        if (stage == Stage::observing) launch({Operation::observe, epoch.load(), source, 0});
        if (stage == Stage::preparing) launch({Operation::refresh, epoch.load(), source, handle});
        if (stage == Stage::authorized) {
            launch({Operation::refresh, epoch.load(), source, handle, live_consumer()});
        }
    }
};

HandoffEligibilityCoordinator::HandoffEligibilityCoordinator(const trust::TrustStore& trust,
    HandoffOperations operations, Listener listener, Timing timing)
    : impl_(std::make_unique<Impl>(trust, std::move(operations), std::move(listener), timing)) {}
HandoffEligibilityCoordinator::~HandoffEligibilityCoordinator() {
    impl_->retire();
    impl_->settle();
}
void HandoffEligibilityCoordinator::begin(helper::Source source) {
    // A retry must not reset a bootstrap that is still progressing: retiring it would deny the
    // helper's live handle and poison its re-observation through retired_handle (#161).
    if (impl_->in_progress() && impl_->source == source && impl_->live_consumer()) return;
    impl_->retire();
    if (!source.generation || !impl_->consumer || impl_->consumer->value != source.generation) return;
    ++impl_->epoch;
    impl_->source = source;
    impl_->stage = Impl::Stage::observing;
    impl_->establishment = Clock::now() + impl_->timing.establishment;
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
bool HandoffEligibilityCoordinator::bootstrapping() const {
    return impl_->in_progress() && Clock::now() < impl_->bootstrap_until + impl_->timing.connect;
}
void HandoffEligibilityCoordinator::tick(bool active) { impl_->active = active; impl_->tick(); }
// An external retirement precedes a fresh attempt (Runtime defer() and admit() via
// WirelessReceiver::stop(), before ReconnectPeerV1 or AcquireApV1). The denial is settled
// synchronously: queued behind an in-flight operation, it would otherwise reach the helper after
// that reconnect, which then finds the retired bootstrap still valid and keeps its binding and
// lease, so the fresh attempt's source is rejected (#169 review). Bounded like any helper call.
void HandoffEligibilityCoordinator::retire() { impl_->retire(); impl_->settle(); }
bool HandoffEligibilityCoordinator::admitting() const { return impl_->valid() && impl_->live_consumer(); }
bool HandoffEligibilityCoordinator::valid_session() const { return impl_->valid(); }
std::optional<trust::PhoneIdentity> HandoffEligibilityCoordinator::phone() const { return impl_->phone; }
std::uint64_t HandoffEligibilityCoordinator::epoch() const { return impl_->epoch.load(); }
std::uint64_t HandoffEligibilityCoordinator::snapshot_generation() const {
    return impl_->snapshot ? impl_->snapshot->generation : 0;
}
}
