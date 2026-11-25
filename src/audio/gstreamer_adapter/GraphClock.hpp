// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ReaderPhase.hpp"
#include <mutex>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace aa::audio::detail {

// Observe completed graph cycles for this Pulse stream. A stalled follower
// stays triggered while the driver continues running. Charge those cycles
// once, when the follower finishes again.
class GraphClock {
public:
    struct Reader {
        std::mutex mutex;
        std::optional<ReaderPhase::Read> sample;
    };
    explicit GraphClock(std::string name, Reader& reader);
    ~GraphClock();
    const std::string name;
    Reader& reader;
    std::atomic<std::int64_t> quantum_ns{};
    std::atomic<bool> available{}, driver_relocking{};
    std::atomic<bool> restart{}; // a new sink epoch: measure the reader afresh
    std::atomic<std::int64_t> held{}, drift{};
    std::atomic<std::uint64_t> disruptions{}, suspends{}, suspend_loss_frames{};
private:
    struct Observer;
    std::unique_ptr<Observer> observer_;
};

}
