// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
    explicit GraphClock(std::string name);
    ~GraphClock();
    const std::string name;
    std::atomic<bool> available{};
    std::atomic<std::int64_t> held{}, drift{};
    std::atomic<std::uint64_t> disruptions{}, suspends{};
private:
    struct Observer;
    std::unique_ptr<Observer> observer_;
};

}
