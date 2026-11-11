/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "timeman.h"

#include <algorithm>

#include "search.h"

namespace Nest {

void TimeManager::init(
  const Search::Limits& limits, Color us, int ply, int moveOverhead, bool ponder) {
    startTime = limits.startTime;

    const TimePoint time = limits.time[us];
    const TimePoint inc  = limits.inc[us];
    if (time == 0)
    {
        optimumTime = maximumTime = 0;
        return;
    }

    // What can be used without touching the safety margin.
    const TimePoint usable = std::max<TimePoint>(1, time - moveOverhead);

    // Expected number of our moves still to play in this time period. Early
    // in the game slightly more are assumed, to keep a reserve.
    const int horizon = limits.movestogo ? std::clamp(limits.movestogo, 1, 40)
                                         : std::clamp(46 - ply / 5, 22, 40);

    double opt = double(usable) / horizon + 0.80 * double(inc);
    // With an explicit move count we can use the period more fully.
    double maxFactor = limits.movestogo ? std::min(1.0 + 0.5 * limits.movestogo, 4.5) : 4.5;

    if (ponder)
        opt *= 1.25;

    const TimePoint hardCap = std::max<TimePoint>(
      1, limits.movestogo == 1 ? TimePoint(0.80 * usable) : TimePoint(0.55 * usable));

    optimumTime = std::clamp<TimePoint>(TimePoint(opt), 1, hardCap);
    maximumTime = std::clamp<TimePoint>(TimePoint(opt * maxFactor), 1, hardCap);
}

}  // namespace Nest
