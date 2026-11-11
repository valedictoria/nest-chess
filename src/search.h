/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "eval.h"
#include "history.h"
#include "misc.h"
#include "position.h"
#include "timeman.h"
#include "types.h"

namespace Nest {

class ThreadPool;
class TranspositionTable;

namespace Search {

struct Limits {
    TimePoint         time[COLOR_NB] = {0, 0}, inc[COLOR_NB] = {0, 0};
    TimePoint         movetime = 0, startTime = 0;
    int               movestogo = 0, depth = 0, mate = 0;
    uint64_t          nodes     = 0;
    bool              infinite = false, ponder = false;
    std::vector<Move> searchmoves;

    bool use_time_management() const { return time[WHITE] || time[BLACK]; }
};

struct RootMove {
    explicit RootMove(Move m) :
        pv(1, m) {}
    bool operator==(Move m) const { return pv[0] == m; }
    // Sort by score, falling back on the previous iteration's score.
    bool operator<(const RootMove& m) const {
        return m.score != score ? m.score < score : m.previousScore < previousScore;
    }

    std::vector<Move> pv;
    Value             score         = -VALUE_INFINITE;
    Value             previousScore = -VALUE_INFINITE;
    Value             averageScore  = -VALUE_INFINITE;
    Value             uciScore      = -VALUE_INFINITE;
    bool              lowerbound = false, upperbound = false;
    int               selDepth = 0;
    uint64_t          effort   = 0;
};

// One line of search output, in engine terms; frontends format it.
struct Info {
    int               depth, selDepth, multiPV;
    Value             score;
    bool              lowerbound, upperbound;
    uint64_t          nodes, nps;
    int               hashfull;
    TimePoint         time;
    std::vector<Move> pv;
};

struct Callbacks {
    std::function<void(const Info&)>     onInfo;
    std::function<void(int, Move, int)>  onCurrMove;  // depth, move, move number
    std::function<void(Move, Move)>      onBestMove;  // best move, ponder move
};

// Engine-wide settings read by the search.
struct Settings {
    int  multiPV      = 1;
    int  moveOverhead = 10;
    bool ponder       = false;
};

struct Stack {
    Move*                     pv;
    PieceToHistory*           continuationHistory;
    PieceToCorrectionHistory* continuationCorrectionHistory;
    int                       ply;
    Move                      currentMove;
    Move                      excludedMove;
    Move                      killers[2];
    Value                     staticEval;
    int                       statScore;
    int                       moveCount;
    int                       cutoffCnt;
    bool                      inCheck;
    bool                      ttPv;
    bool                      ttHit;
};

enum NodeType { NonPV, PV, Root };

// Everything one search thread owns. Only the transposition table, the stop
// flags and (for reporting) the node counters are shared between workers.
class alignas(64) Worker {
   public:
    Worker(ThreadPool& pool, TranspositionTable& table, size_t index);

    void clear();
    void start_searching();
    bool is_main() const { return idx == 0; }

    Position              rootPos;
    StateInfo             rootState;
    std::vector<RootMove> rootMoves;
    Limits                limits;
    Depth                 rootDepth = 0, completedDepth = 0;
    int                   selDepth = 0;

    // Written by this thread only, read by others for reporting.
    alignas(64) std::atomic<uint64_t> nodes{0};
    std::atomic<uint64_t>             bestMoveChanges{0};

   private:
    void iterative_deepening();

    template<NodeType NT>
    Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);
    template<NodeType NT>
    Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta);

    void do_move(Position& pos, Move move, StateInfo& st, bool givesCheck, Stack* ss);
    void undo_move(Position& pos, Move move);

    int   correction_value(const Position& pos, const Stack* ss) const;
    void  update_correction(const Position& pos, Stack* ss, int bonus);
    Value evaluate(const Position& pos);
    int   reduction(bool improving, Depth d, int moveNumber, int delta) const;

    void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Square prevSq,
                          const Move* quiets, int quietCount, const Move* captures, int captureCount,
                          Depth depth, bool isTTMove);
    void update_quiet_histories(const Position& pos, Stack* ss, Move move, int bonus);
    void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);

    void check_time();
    void report(Depth depth);

    // Main-thread time management state.
    TimeManager tm;
    int         callsCnt             = 0;
    bool        stopOnPonderhit      = false;
    double      previousTimeReduction = 1.0;
    Value       bestPreviousScore        = VALUE_INFINITE;
    Value       bestPreviousAverageScore = VALUE_INFINITE;
    Value       iterValue[4]             = {};
    TimePoint   lastInfoTime             = 0;

    ThreadPool&         threads;
    TranspositionTable& tt;
    size_t              idx;

    size_t   pvIdx = 0, pvLast = 0;
    Value    rootDelta = 0;
    int      optimism[COLOR_NB] = {0, 0};
    int      nmpMinPly = 0;
    int      reductions[MAX_MOVES];

    AccumulatorStack accumulators;

    ButterflyHistory              mainHistory;
    CaptureHistory                captureHistory;
    ContinuationHistory           continuationHistory[2][2];  // [in check][capture]
    PawnHistory                   pawnHistory;
    CounterMoves                  counterMoves;
    PawnCorrectionHistory         pawnCorrection;
    NonPawnCorrectionHistory      nonPawnCorrection;
    ContinuationCorrectionHistory continuationCorrection;

    friend class Nest::ThreadPool;
};

}  // namespace Search
}  // namespace Nest
