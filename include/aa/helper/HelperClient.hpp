// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>

#include <cstdint>

namespace aa::helper {

// Client contract for the privileged sidecar (aa-system-helper.service). The
// receiver never touches NetworkManager, USB policy, ModemManager or BlueZ
// directly; every privileged effect is a typed operation sent through this
// client to the helper process.
//
// Ownership/threading: the client is owned by the session and called from the
// session thread; the private RPC adapter (D-Bus or AF_UNIX, task 34) may block
// up to the caller's timeout but must honour cancellation. The transport and
// wire format of that RPC are private adapter details and appear in no public
// header.
//
// Error contract: helper_unavailable (helper absent/busy), helper_rejected
// (helper policy refused), helper_protocol_mismatch (version skew) and
// core::cancelled / core::timeout. Privileged effects always fail closed.
enum class Operation {
    ensure_access_point,
    teardown_access_point,
    register_bluez_profile,
    unregister_bluez_profile,
    apply_usb_policy,
    exclude_modem_manager,
    open_pairing_window,
};

struct Request final {
    Operation op{};
    std::uint8_t channel{36};  // only meaningful for ensure_access_point
};

struct Response final {
    bool applied{false};
};

class HelperClient {
public:
    virtual ~HelperClient() = default;
    HelperClient() = default;
    HelperClient(const HelperClient&) = delete;
    HelperClient& operator=(const HelperClient&) = delete;
    HelperClient(HelperClient&&) = delete;
    HelperClient& operator=(HelperClient&&) = delete;

    virtual core::Result<Response> call(const Request& request,
                                        core::Milliseconds timeout,
                                        core::CancellationToken cancellation) = 0;
};

} // namespace aa::helper
