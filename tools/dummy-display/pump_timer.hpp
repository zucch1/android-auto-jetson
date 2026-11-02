// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Video pump cadence for the shipped aa-dummy-display (#60, #79). Each tick
// drains the helper and presents ready frames, so the interval bounds the
// schedule-to-present delay a frame can wait. The 1800 s / 7200 s A/V
// qualification passed only with the 5 ms precise pump of display 802cd63e
// (v17 main.cpp, SHA256 488929cb...f09f8ba); the earlier coarse 30 ms pump
// held the video schedule-to-present median near 51 ms (vs 11 ms) and failed
// A/V at about 135 ms against the 50 ms gate.

#include <QTimer>

#include <chrono>

namespace aa::display::pump {

inline constexpr std::chrono::milliseconds kInterval{5};
inline constexpr Qt::TimerType kTimerType = Qt::PreciseTimer;

// Applies the qualified cadence; the caller still decides when to start().
inline void configure(QTimer& timer) {
    timer.setTimerType(kTimerType);
    timer.setInterval(kInterval);
}

}  // namespace aa::display::pump
