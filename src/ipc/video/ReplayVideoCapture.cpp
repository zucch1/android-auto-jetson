// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ReplayVideoCapture.hpp>

#include <cstddef>

namespace aa::ipc {

core::Result<void> ReplayVideoCapture::on_access_unit(const VideoAccessUnit& unit) {
    replay::VideoRecord record{};
    record.frame = unit.frame;
    record.sequence = unit.sequence;
    record.timestamp = core::Nanoseconds{static_cast<std::int64_t>(unit.timestamp_ns)};
    record.idr = unit.idr;
    record.payload.reserve(unit.payload.size());
    for (const std::uint8_t byte : unit.payload) {
        record.payload.push_back(static_cast<std::byte>(byte));
    }
    return recorder_->record_video(record.timestamp, record);
}

} // namespace aa::ipc
