// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/tls/Credentials.hpp>
#include <functional>

namespace aa::tls {
enum class Mode { encryption_only_compatibility, verified_peer_test_only };

// The caller binds this predicate to its transport/session phone identity.
// No TLS certificate or environment flag can stand in for phone approval.
class Policy final {
public:
    explicit Policy(std::function<bool()> approved_phone = {},
                    Mode mode = Mode::encryption_only_compatibility);
    void require_approved() const;
    void configure(SSL* ssl) const;
    [[nodiscard]] const char* label() const noexcept;
private:
    std::function<bool()> approved_phone_;
    Mode mode_;
};
}
