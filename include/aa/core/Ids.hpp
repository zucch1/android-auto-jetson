// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <compare>
#include <cstdint>

namespace aa::core {

// Strong semantic identifiers: values of different id spaces never mix at
// compile time. Zero is the "unset" id and converts to false; real ids are
// allocated by the owning module (session ids by the session state machine,
// consumer ids by the IPC control surface, and so on).
enum class IdSpace { session, consumer, phone, pairing_request, frame };

template <IdSpace Space>
struct Id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }

    [[nodiscard]] friend constexpr bool operator==(Id, Id) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(Id, Id) noexcept = default;
};

using SessionId = Id<IdSpace::session>;
using ConsumerId = Id<IdSpace::consumer>;
using PhoneId = Id<IdSpace::phone>;
using PairingRequestId = Id<IdSpace::pairing_request>;
using FrameId = Id<IdSpace::frame>;

} // namespace aa::core
