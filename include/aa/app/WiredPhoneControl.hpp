// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/trust/Trust.hpp>

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace aa::trust { class ApprovedPhoneStore; }

namespace aa::app {
class WiredSession;

class WiredPhoneControl final {
  public:
    WiredPhoneControl(trust::ApprovedPhoneStore&, WiredSession&);
    void bind_owner();
    void bind_phone(const trust::PhoneIdentity&);
    [[nodiscard]] core::Result<void> forget(core::PhoneId);
    void checkpoint();
    [[nodiscard]] bool finish();
    void close();

  private:
    struct Request {
        core::PhoneId phone;
        std::promise<core::Result<void>> reply;
    };
    void assert_owner();
    void complete(std::unique_ptr<Request>, core::Result<void>);

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
};
} // namespace aa::app
