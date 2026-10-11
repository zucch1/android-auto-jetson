// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/replay/Recorder.hpp>

namespace aa::ipc {

// Delivers every reassembled access unit to the task-20 recorder, producing
// replayable aa-replay-fixture-v1 stream captures. Recorder bounds (record
// count, payload bytes, monotonic time) are enforced by the recorder itself
// and surface as its typed errors; a capture failure does not corrupt the
// channel state.
class ReplayVideoCapture final : public VideoStreamCapture {
public:
    explicit ReplayVideoCapture(replay::Recorder& recorder) noexcept
        : recorder_(&recorder) {}

    ReplayVideoCapture(const ReplayVideoCapture&) = delete;
    ReplayVideoCapture& operator=(const ReplayVideoCapture&) = delete;
    ReplayVideoCapture(ReplayVideoCapture&&) = delete;
    ReplayVideoCapture& operator=(ReplayVideoCapture&&) = delete;

    [[nodiscard]] core::Result<void> on_access_unit(const VideoAccessUnit& unit) override;

private:
    replay::Recorder* recorder_;
};

} // namespace aa::ipc
