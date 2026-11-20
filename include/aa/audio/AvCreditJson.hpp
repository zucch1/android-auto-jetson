// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/AvCreditLedger.hpp>

#include <string>

namespace aa::audio {

[[nodiscard]] const char* av_credit_name(AvCreditEvent event) noexcept;
[[nodiscard]] std::string av_delivery_json(const channels::AvDeliveryId& value);
[[nodiscard]] std::string av_observation_json(const AvObservation& value);
[[nodiscard]] std::string av_credit_state_json(const AvCreditState& state);
[[nodiscard]] std::string av_totals_json(const AvGenerationTotals& value);
[[nodiscard]] std::string av_credit_json(const AvCreditSnapshot& value);

} // namespace aa::audio
