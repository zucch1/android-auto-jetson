// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Single supervisor-owned video connection for the lifetime of a managed
// projection request. Workers (out-of-process wired worker via SCM_RIGHTS,
// in-process wireless Runtime via borrow()) borrow the socket without SHUT_WR
// ownership while sharing monotonic frame/sequence counters across restarts
// and wired<->wireless handoffs.

#include <aa/core/Error.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/VideoChannel.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace aa::wireless_receiver {

class ManagedVideoBroker final {
public:
    using ErrorReporter = std::function<void(const char*, ErrorCode)>;

    ManagedVideoBroker() = default;

    void set_error_reporter(ErrorReporter reporter) {
        on_error_ = std::move(reporter);
    }

    // Non-blocking poll of the owned endpoint and listener:
    // 1. Detect peer loss on the held connection and close it cleanly.
    // 2. Poll the listener (0 ms accept wait, 2 s internal pending-handshake
    //    deadline). Ensures the server prepares its size-sealed shared
    //    stream-counter memfd BEFORE sending its AAL1 record so counter setup
    //    failures close the socket prior to handshake completion and surface a
    //    receiver diagnostic instead of post-AAL1 EOF.
    // Returns true when the held connection changed (cleared or replaced).
    bool poll(ipc::VideoServer& server) {
        bool changed = false;
        if (connection_.has_value() && connection_->peer_closed()) {
            connection_->close();
            connection_.reset();
            state_fd_ = -1;
            ++generation_;
            changed = true;
        }
        if (!server.closed()) {
            server.set_prepare_shared_state(true);
            auto accepted = server.accept_consumer(core::Milliseconds{0});
            if (accepted.has_value()) {
                auto prepared = accepted.value().prepare_shared_state();
                if (prepared.has_value()) {
                    if (connection_.has_value()) {
                        connection_->close();
                    }
                    connection_.emplace(std::move(accepted.value()));
                    state_fd_ = prepared.value();
                    ++generation_;
                    changed = true;
                } else {
                    report_error("video shared state", prepared.error().code());
                    accepted.value().close();
                }
            } else if (accepted.error().code() != ErrorCode::timeout) {
                report_error("video consumer accept", accepted.error().code());
            }
        }
        return changed;
    }

    // End the current consumer connection (marks retired + sends SHUT_WR -> clean
    // EOF), abort any pending half-completed handshake, and drain any connections
    // already queued in the listener's kernel accept queue from the stopped request.
    // Returns an error if the listener queue could not be confirmed empty.
    [[nodiscard]] core::Result<void> release_connection(ipc::VideoServer* server = nullptr) noexcept {
        if (connection_.has_value()) {
            connection_->close();
            connection_.reset();
            state_fd_ = -1;
            ++generation_;
        }
        if (server != nullptr) {
            if (auto drained = server->drain_queued_connections(); !drained) {
                return drained.error();
            }
        }
        return {};
    }

    [[nodiscard]] bool has_connection() const noexcept {
        return connection_.has_value() && !connection_->closed() && state_fd_ >= 0;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] bool has_pending_handshake(const ipc::VideoServer& server) const noexcept {
        return server.has_pending_handshake();
    }

    [[nodiscard]] core::Result<ipc::VideoProducer> borrow() {
        if (!has_connection()) {
            return Error{ErrorCode::transport_closed};
        }
        return connection_->borrow();
    }

    [[nodiscard]] int client_fd() const noexcept {
        return has_connection() ? connection_->native_handle() : -1;
    }

    [[nodiscard]] int state_fd() const noexcept {
        return has_connection() ? state_fd_ : -1;
    }

    [[nodiscard]] const ipc::NegotiatedLimits& limits() const noexcept {
        return connection_->limits();
    }

private:
    void report_error(const char* what, ErrorCode code) const {
        if (on_error_) {
            on_error_(what, code);
        }
    }

    std::optional<ipc::VideoProducer> connection_{};
    int state_fd_{-1};
    std::uint64_t generation_{0};
    ErrorReporter on_error_{};
};

} // namespace aa::wireless_receiver
