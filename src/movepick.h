/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include "history.h"
#include "movegen.h"
#include "position.h"
#include "types.h"

namespace Nest {

// Hands out the moves of a position one at a time, most promising first,
// generating each class of moves only when it is reached:
//   TT move, good captures, killers and counter move, quiets by history,
//   losing captures, poor quiets.
// All moves returned are legal.
class MovePicker {
   public:
    MovePicker(const MovePicker&)            = delete;
    MovePicker& operator=(const MovePicker&) = delete;

    // Main search and quiescence search (depth <= 0: captures only, or all
    // evasions when in check).
    MovePicker(const Position&         pos,
               Move                    ttMove,
               Depth                   depth,
               const ButterflyHistory* mainHistory,
               const CaptureHistory*   captureHistory,
               const PieceToHistory**  continuationHistory,
               const PawnHistory*      pawnHistory,
               const Move*             killers,
               Move                    counterMove);

    // ProbCut: captures whose static exchange value is at least `threshold`.
    MovePicker(const Position& pos, Move ttMove, int threshold, const CaptureHistory* captureHistory);

    Move next_move();
    void skip_quiet_moves() { skipQuiets = true; }

   private:
    template<typename Pred>
    Move select(Pred pred);
    void score_captures();
    void score_quiets();
    void score_evasions();

    const Position&         pos;
    const ButterflyHistory* mainHistory         = nullptr;
    const CaptureHistory*   captureHistory      = nullptr;
    const PieceToHistory**  continuationHistory = nullptr;
    const PawnHistory*      pawnHistory         = nullptr;
    Move                    ttMove;
    Move                    refutations[3];
    ExtMove *               cur, *endMoves, *endBadCaptures, *beginBadQuiets, *endBadQuiets;
    int                     stage;
    int                     threshold = 0;
    Depth                   depth     = 0;
    bool                    skipQuiets = false;
    ExtMove                 moves[MAX_MOVES];
};

}  // namespace Nest
