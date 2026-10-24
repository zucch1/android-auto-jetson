// SPDX-License-Identifier: GPL-3.0-or-later
    void run() {
        std::uint64_t attempted = 0;
        std::uint64_t observed_generation = 0;
        auto next_regulatory = Clock::now();
        auto next_revoke = Clock::now();
        for (;;) {
            bool request{}, quit{}, revoke{}, pending_revocation{};
            std::uint64_t epoch{};
            { std::unique_lock lock{mutex};
              changed.wait_for(lock, std::chrono::milliseconds(100), [this, observed_generation, next_revoke] {
                  return exiting || snapshot_job || fence_job || generation != observed_generation ||
                      (revoke_pending && Clock::now() >= next_revoke);
              });
              request = wanted; quit = exiting; epoch = generation; observed_generation = epoch;
              pending_revocation = revoke_pending;
              revoke = pending_revocation && Clock::now() >= next_revoke;
            }
            if (quit) { (void)stop(); return; }
            std::shared_ptr<SnapshotJob> job;
            { const std::lock_guard lock{mutex}; job = std::move(snapshot_job); }
            if (job) {
                core::Result<ApSnapshot> result{Error{ErrorCode::helper_unavailable}};
                if (!job->deadline.expired() && current(epoch) && activation.generation == epoch &&
                    !activation.password.empty() && connection && actual_channel(policy, *commands)) {
                    connection->collection_deadline(job->deadline);
                    const auto first = connection->endpoint(policy, device);
                    const auto second = connection->endpoint(policy, device);
                    if (first && second && first.value() == second.value() && actual_channel(policy, *commands) &&
                        current(epoch) && !job->deadline.expired()) {
                        auto snapshot = activation;
                        snapshot.bssid = first.value().first; snapshot.ip = first.value().second;
                        const std::lock_guard lock{mutex};
                        if (generation == epoch && wanted && !exiting && !revoke_pending && state == State::active) {
                            result = snapshot;
                            validated_epoch = epoch; validated_endpoint = first.value(); validated_deadline = job->authority_deadline;
                        }
                    }
                    if (!result && !connection->identity_current(policy, device)) {
                        const std::lock_guard lock{mutex};
                        validated_epoch = 0;
                        if (generation == epoch) { ++generation; wanted = false; state = State::unavailable; }
                    }
                    connection->collection_deadline({});
                }
                job->result.set_value(std::move(result));
            }
            std::shared_ptr<FenceJob> fence;
            { const std::lock_guard lock{mutex}; fence = std::move(fence_job); }
            if (fence) {
                bool sent{};
                connection->collection_deadline(fence->deadline);
                const auto endpoint = !fence->deadline.expired() && current(fence->epoch) ?
                    connection->endpoint(policy, device) : core::Result<std::pair<std::string, std::string>>{Error{ErrorCode::helper_unavailable}};
                const bool channel_current = endpoint && actual_channel(policy, *commands);
                {
                    const std::lock_guard lock{mutex};
                    if (channel_current && endpoint.value() == validated_endpoint && validated_epoch == fence->epoch &&
                        generation == fence->epoch && wanted && !exiting && !revoke_pending && state == State::active &&
                        !fence->deadline.expired()) {
                        fence->send(); sent = true;
                    } else {
                        validated_epoch = 0;
                        if (generation == fence->epoch) { ++generation; wanted = false; state = State::unavailable; }
                    }
                }
                connection->collection_deadline({});
                fence->result.set_value(sent);
            }
            if (pending_revocation && !revoke) continue;
            if (revoke) {
                if (protected_interface && !quarantined) {
                    quarantined = commands->run({"/usr/sbin/nft", "-f", "-"},
                        "flush chain inet aa_receiver_ap input\nadd rule inet aa_receiver_ap input iifname \"" +
                        policy.interface + "\" drop\n").has_value();
                }
                if (!rotated) rotated = credentials(policy, true).has_value();
                const bool safe = stop();
                const std::lock_guard lock{mutex};
                state = safe && rotated ? State::inactive : State::unavailable;
                if (safe && rotated) { revoke_pending = false; revoke_done = true; changed.notify_all(); }
                next_revoke = Clock::now() + std::chrono::milliseconds(100);
                continue;
            }
            if (!request) {
                attempted = epoch;
                publish(!protected_interface || stop() ? State::inactive : State::unavailable);
                continue;
            }
            if (attempted != epoch) {
                attempted = epoch;
                if (!start(epoch)) {
                    (void)stop();
                    publish(State::unavailable);
                    std::cerr << "system-helper: AP startup failed; check jurisdiction, channel36, NM and isolation\n";
                }
                continue;
            }
            State observed;
            { const std::lock_guard lock{mutex}; observed = state; }
            bool regulatory_ok = true;
            if (observed == State::active && Clock::now() >= next_regulatory) {
                regulatory_ok = eligible(policy, *commands);
                next_regulatory = Clock::now() + std::chrono::seconds(1);
            }
            if (observed == State::active && (!regulatory_ok || !connection->healthy() || !connection->active() ||
                !actual_channel(policy, *commands))) {
                publish(State::unavailable); (void)stop();
                std::cerr << "system-helper: AP lost eligibility/channel/owner; rolled back\n";
            } else if (observed == State::unavailable && protected_interface) (void)stop();
        }
    }
