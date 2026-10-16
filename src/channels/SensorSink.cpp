// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/SensorSink.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/Sensors.hpp>

namespace aa::channels {

core::Result<void> SensorSink::on_message(const protocol::MessageView& message) {
    if (message.header.channel != protocol::ChannelRole::sensors) {
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    const auto request = protocol::decode_sensor_request(message.payload);
    if (!request) { return request.error(); }
    // All non-night types map to an unsupported policy channel, never to a
    // vehicle provider (none is installed). Unknown wire enums follow suit.
    const auto channel = request.value().type == static_cast<std::int32_t>(protocol::SensorKind::night_mode)
        ? input::SensorChannel::night_mode : input::SensorChannel::location;
    const auto response = service_.submit_request({channel, request.value().min_update_period_ms});
    const bool accepted = response.status == input::SensorRequestStatus::accepted;
    auto payload = protocol::encode_sensor_response(accepted);
    if (!payload) { return payload.error(); }
    if (auto sent = send_(std::move(payload).value()); !sent) { return sent.error(); }
    if (!accepted) { return {}; }
    auto batch = protocol::encode_night_mode_batch(service_.night_mode());
    if (!batch) { return batch.error(); }
    return send_(std::move(batch).value());
}

void SensorSink::on_sensor_event(const input::SensorEvent& event) {
    if (event.channel != input::SensorChannel::night_mode
        || event.origin != input::SensorOrigin::manual || (event.value != 0 && event.value != 1)) {
        event_error_ = Error{ErrorCode::channel_unsupported_capability};
        return;
    }
    auto payload = protocol::encode_night_mode_batch(event.value == 1);
    if (!payload) { event_error_ = payload.error(); return; }
    if (auto sent = send_(std::move(payload).value()); !sent) { event_error_ = sent.error(); }
}

core::Result<void> SensorSink::set_night_mode(bool night) {
    event_error_.reset();
    service_.set_night_mode(night);
    if (event_error_) { return *event_error_; }
    return {};
}

} // namespace aa::channels
