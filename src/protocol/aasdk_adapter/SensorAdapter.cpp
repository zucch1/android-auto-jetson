// SPDX-License-Identifier: GPL-3.0-or-later
#include "Framing.hpp"

#include <aa/protocol/Sensors.hpp>

#include <aap_protobuf/service/sensorsource/SensorMessageId.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorRequest.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorResponse.pb.h>
#include <google/protobuf/unknown_field_set.h>

#include <limits>

namespace aa::protocol {
namespace source = aap_protobuf::service::sensorsource;
namespace sensor = source::message;
namespace shared = aap_protobuf::shared;

static_assert(source::SENSOR_MESSAGE_REQUEST == 32769);
static_assert(source::SENSOR_MESSAGE_RESPONSE == 32770);
static_assert(source::SENSOR_MESSAGE_BATCH == 32771);
static_assert(source::SENSOR_MESSAGE_ERROR == 32772);
static_assert(sensor::SENSOR_NIGHT_MODE == 10);
static_assert(shared::STATUS_SUCCESS == 0);
static_assert(shared::STATUS_INVALID_SENSOR == -9);

core::Result<SensorStartRequest> decode_sensor_request(std::span<const std::byte> payload) {
    if (payload.size() < 2 || payload[0] != std::byte{0x80} || payload[1] != std::byte{0x01}
        || payload.size() - 2 > aasdk_adapter::kMaxProtoBodyBytes) {
        return aasdk_adapter::malformed();
    }
    sensor::SensorRequest wire;
    const auto body = payload.subspan(2);
    // Proto2 keeps unknown enum values outside the required type field. Parse
    // partially so an unknown sensor still gets the deterministic unsupported
    // response, while missing fields and malformed types remain errors.
    if (!wire.ParsePartialFromArray(body.data(), static_cast<int>(body.size()))
        || !wire.has_min_update_period() || wire.min_update_period() < 0) {
        return aasdk_adapter::malformed();
    }
    std::int32_t type = wire.has_type() ? static_cast<std::int32_t>(wire.type()) : 0;
    bool has_type = wire.has_type();
    const auto& unknown = wire.GetReflection()->GetUnknownFields(wire);
    for (int index = 0; index < unknown.field_count(); ++index) {
        const auto& field = unknown.field(index);
        if (field.number() != 1 || has_type
            || field.type() != google::protobuf::UnknownField::TYPE_VARINT
            || field.varint() > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return aasdk_adapter::malformed();
        }
        type = static_cast<std::int32_t>(field.varint());
        has_type = true;
    }
    if (!has_type) { return aasdk_adapter::malformed(); }
    return SensorStartRequest{type, static_cast<std::uint64_t>(wire.min_update_period())};
}

core::Result<std::vector<std::byte>> encode_sensor_response(bool accepted) {
    sensor::SensorResponse wire;
    wire.set_status(accepted ? shared::STATUS_SUCCESS : shared::STATUS_INVALID_SENSOR);
    return aasdk_adapter::framed_id(source::SENSOR_MESSAGE_RESPONSE, wire);
}

core::Result<std::vector<std::byte>> encode_night_mode_batch(bool night) {
    sensor::SensorBatch wire;
    wire.add_night_mode_data()->set_night_mode(night);
    return aasdk_adapter::framed_id(source::SENSOR_MESSAGE_BATCH, wire);
}

} // namespace aa::protocol
