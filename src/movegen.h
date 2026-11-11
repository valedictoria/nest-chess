/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <algorithm>

#include "types.h"

namespace Nest {

class Position;

// Every generator produces strictly legal moves (check and pin masks are
// applied during generation), whatever the type:
//   CAPTURES  captures, queen promotions, capturing under-promotions
//   QUIETS    everything else
//   EVASIONS  all moves; meant for positions in check
//   LEGAL     all moves
// CAPTURES + QUIETS == LEGAL in any position, in check or not.
enum GenType { CAPTURES, QUIETS, EVASIONS, LEGAL };

struct ExtMove {
    Move move;
    int  value;

    operator Move() const { return move; }
    void operator=(Move m) { move = m; }
};

inline bool operator<(const ExtMove& a, const ExtMove& b) { return a.value < b.value; }

template<GenType T>
ExtMove* generate(const Position& pos, ExtMove* moveList);

template<GenType T>
struct MoveList {
    explicit MoveList(const Position& pos) :
        last(generate<T>(pos, moveList)) {}
    const ExtMove* begin() const { return moveList; }
    const ExtMove* end() const { return last; }
    size_t         size() const { return size_t(last - moveList); }
    bool           contains(Move m) const {
        for (const ExtMove* it = moveList; it != last; ++it)
            if (it->move == m)
                return true;
        return false;
    }

   private:
    ExtMove moveList[MAX_MOVES], *last;
};

}  // namespace Nest
