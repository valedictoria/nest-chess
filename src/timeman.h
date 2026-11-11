/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include "misc.h"
#include "types.h"

namespace Nest {

namespace Search {
struct Limits;
}

// Turns the clock situation into two budgets for this move: `optimum`, the
// time we would like to spend, and `maximum`, which is never exceeded.
class TimeManager {
   public:
    void      init(const Search::Limits& limits, Color us, int ply, int moveOverhead, bool ponder);
    TimePoint optimum() const { return optimumTime; }
    TimePoint maximum() const { return maximumTime; }
    TimePoint elapsed() const { return now() - startTime; }

   private:
    TimePoint startTime   = 0;
    TimePoint optimumTime = 0;
    TimePoint maximumTime = 0;
};

}  // namespace Nest
