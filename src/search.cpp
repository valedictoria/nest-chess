/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.

  The overall shape of this search (principal variation search with
  aspiration windows, the order of the pruning steps, late move reductions
  driven by history, singular extensions, correction history) follows the
  design that Stockfish (GPL-3.0) has made standard. The code is our own.
*/

#include "search.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "movegen.h"
#include "movepick.h"
#include "thread.h"
#include "tt.h"
#include "tune.h"

namespace Nest::Search {

namespace {

// ---- tunable constants (see tune.h) -----------------------------------------

TUNABLE(AspirationDelta, 5, 1, 30)
TUNABLE(AspirationDiv, 11134, 2000, 40000)
TUNABLE(OptimismMul, 132, 0, 300)
TUNABLE(OptimismDiv, 89, 20, 300)

TUNABLE(RazorBase, 462, 100, 900)
TUNABLE(RazorDepth2, 297, 50, 600)
TUNABLE(RfpDepth, 14, 4, 20)
TUNABLE(RfpMargin, 112, 40, 200)
TUNABLE(RfpNoTtCut, 26, 0, 80)
TUNABLE(RfpStatDiv, 310, 100, 800)
TUNABLE(NmpEvalBase, 418, 100, 800)
TUNABLE(NmpEvalDepth, 21, 5, 50)
TUNABLE(NmpEvalDiv, 235, 80, 500)
TUNABLE(NmpBaseR, 5, 2, 8)
TUNABLE(ProbCutMargin, 187, 80, 400)
TUNABLE(ProbCutImproving, 56, 0, 150)
TUNABLE(ProbCutTTMargin, 417, 150, 800)

TUNABLE(LmrBase, 1132, 0, 3000)
TUNABLE(LmrScale, 2143, 1200, 3200)
TUNABLE(LmrDelta, 735, 0, 2000)
TUNABLE(LmrNotImproving, 191, 0, 512)
TUNABLE(LmrTtPv, 1037, 0, 3000)
TUNABLE(LmrTtPvValue, 965, 0, 3000)
TUNABLE(LmrTtPvDepth, 960, 0, 3000)
TUNABLE(LmrPvNode, 1018, 0, 3000)
TUNABLE(LmrCutNode, 2355, 0, 5000)
TUNABLE(LmrCutNodeTt, 1087, 0, 3000)
TUNABLE(LmrTtCapture, 1119, 0, 3000)
TUNABLE(LmrCutoffCnt, 991, 0, 3000)
TUNABLE(LmrTtMove, 1937, 0, 4000)
TUNABLE(LmrCheck, 0, 0, 2000)
TUNABLE(LmrHistDiv, 10898, 4000, 30000)
TUNABLE(LmrCaptHistOffset, 4666, 0, 12000)
TUNABLE(LmrQuietHistOffset, 3874, 0, 12000)

TUNABLE(FutCaptBase, 271, 50, 600)
TUNABLE(FutCaptDepth, 243, 50, 600)
TUNABLE(SeeCaptDepth, 166, 50, 400)
TUNABLE(HistPruneDepth, 4107, 1000, 10000)
TUNABLE(HistLmrDiv, 3576, 1000, 10000)
TUNABLE(FutQuietBase, 135, 20, 400)
TUNABLE(FutQuietBest, 49, 0, 200)
TUNABLE(FutQuietDepth, 150, 30, 400)
TUNABLE(SeeQuietDepth2, 26, 5, 80)

TUNABLE(SingularMargin, 56, 10, 160)
TUNABLE(SingularMarginTtPv, 79, 0, 200)
TUNABLE(DoubleExtPv, 249, 0, 600)
TUNABLE(DoubleExtQuiet, 194, 0, 600)
TUNABLE(TripleExtBase, 94, 0, 600)
TUNABLE(TripleExtPv, 287, 0, 600)
TUNABLE(TripleExtQuiet, 249, 0, 600)
TUNABLE(TripleExtTtPv, 99, 0, 600)

TUNABLE(BonusMul, 168, 50, 400)
TUNABLE(BonusOffset, 100, 0, 400)
TUNABLE(BonusMax, 1718, 500, 4000)
TUNABLE(MalusMul, 768, 200, 1500)
TUNABLE(MalusOffset, 257, 0, 800)
TUNABLE(MalusMax, 2351, 800, 5000)

TUNABLE(QsFutility, 301, 100, 600)
TUNABLE(QsSee, 83, 0, 250)

TUNABLE(CorrPawn, 6384, 0, 16000)
TUNABLE(CorrNonPawn, 6725, 0, 16000)
TUNABLE(CorrCont, 5880, 0, 16000)

// ---- small helpers ------------------------------------------------------------

constexpr bool is_win(Value v) { return v >= VALUE_TB_WIN_IN_MAX_PLY; }
constexpr bool is_loss(Value v) { return v <= VALUE_TB_LOSS_IN_MAX_PLY; }

int stat_bonus(Depth d) { return std::min(BonusMul * d - BonusOffset, BonusMax); }
int stat_malus(Depth d) { return std::min(MalusMul * d - MalusOffset, MalusMax); }

// A draw score with a little noise, so that the search does not become
// blind to repetitions that are in fact avoidable.
Value value_draw(uint64_t nodes) { return VALUE_DRAW - 1 + Value(nodes & 2); }

Value to_corrected(Value v, int correction) {
    return std::clamp(v + correction / 131072, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

// Mate scores are stored relative to the node, not to the root.
Value value_to_tt(Value v, int ply) {
    return is_win(v) ? v + ply : is_loss(v) ? v - ply : v;
}

// The inverse; also downgrades mate scores that the 50-move rule could
// prevent from being reached.
Value value_from_tt(Value v, int ply, int r50c) {
    if (v == VALUE_NONE)
        return VALUE_NONE;
    if (is_win(v))
    {
        if (v >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - v > 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;
        return v - ply;
    }
    if (is_loss(v))
    {
        if (v <= VALUE_MATED_IN_MAX_PLY && VALUE_MATE + v > 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;
        return v + ply;
    }
    return v;
}

void update_pv(Move* pv, Move move, const Move* childPv) {
    for (*pv++ = move; childPv && *childPv != Move::none();)
        *pv++ = *childPv++;
    *pv = Move::none();
}

int futility_move_count(bool improving, Depth depth) { return (3 + depth * depth) / (2 - improving); }

int futility_margin(Depth d, bool noTtCutNode, bool improving, bool oppWorsening) {
    const int mult = RfpMargin - RfpNoTtCut * noTtCutNode;
    return mult * d - improving * mult * 2 - oppWorsening * mult / 3;
}

}  // namespace

Worker::Worker(ThreadPool& pool, TranspositionTable& table, size_t index) :
    threads(pool),
    tt(table),
    idx(index) {
    clear();
}

void Worker::clear() {
    mainHistory.fill(0);
    captureHistory.fill(-700);
    pawnHistory.fill(-1100);
    pawnCorrection.fill(0);
    nonPawnCorrection.fill(0);
    continuationCorrection.fill(0);
    for (auto& perCheck : continuationHistory)
        for (auto& perCapture : perCheck)
            perCapture.fill(-400);
    for (auto& perPiece : counterMoves)
        perPiece.fill(Move::none());

    for (int i = 1; i < MAX_MOVES; ++i)
        reductions[i] = int(LmrScale / 100.0 * std::log(i));
    reductions[0] = 0;

    previousTimeReduction    = 1.0;
    bestPreviousScore        = VALUE_INFINITE;
    bestPreviousAverageScore = VALUE_INFINITE;
}

int Worker::reduction(bool improving, Depth d, int moveNumber, int delta) const {
    const int scale = reductions[d] * reductions[std::min(moveNumber, MAX_MOVES - 1)];
    return scale - delta * LmrDelta / std::max(rootDelta, 1) + !improving * scale * LmrNotImproving / 512
         + LmrBase;
}

Value Worker::evaluate(const Position& pos) {
    return Eval::evaluate(pos, accumulators, optimism[pos.side_to_move()]);
}

int Worker::correction_value(const Position& pos, const Stack* ss) const {
    const Color us = pos.side_to_move();
    const Move  m  = (ss - 1)->currentMove;

    const int pcv = pawnCorrection[correction_index(pos.pawn_key())][us];
    const int wnp = nonPawnCorrection[correction_index(pos.non_pawn_key(WHITE))][WHITE][us];
    const int bnp = nonPawnCorrection[correction_index(pos.non_pawn_key(BLACK))][BLACK][us];
    const int cnt =
      m.is_ok() ? int((*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()]) : 0;

    return CorrPawn * pcv + CorrNonPawn * (wnp + bnp) + CorrCont * cnt;
}

void Worker::update_correction(const Position& pos, Stack* ss, int bonus) {
    const Color us = pos.side_to_move();
    const Move  m  = (ss - 1)->currentMove;

    pawnCorrection[correction_index(pos.pawn_key())][us] << bonus;
    nonPawnCorrection[correction_index(pos.non_pawn_key(WHITE))][WHITE][us] << bonus;
    nonPawnCorrection[correction_index(pos.non_pawn_key(BLACK))][BLACK][us] << bonus;
    if (m.is_ok())
        (*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()] << bonus;
}

void Worker::do_move(Position& pos, Move move, StateInfo& st, bool givesCheck, Stack* ss) {
    const bool  capture = pos.capture_stage(move);
    const Piece pc      = pos.moved_piece(move);

    DirtyPiece dp;
    pos.do_move(move, st, givesCheck, dp);
    // Start fetching the child's table entry while the evaluation runs.
    tt.prefetch(pos.key());
    accumulators.push(dp);

    nodes.store(nodes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

    ss->currentMove                   = move;
    ss->continuationHistory           = &continuationHistory[ss->inCheck][capture][pc][move.to_sq()];
    ss->continuationCorrectionHistory = &continuationCorrection[pc][move.to_sq()];
}

void Worker::undo_move(Position& pos, Move move) {
    accumulators.pop();
    pos.undo_move(move);
}

void Worker::update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {
    for (int i : {1, 2, 3, 4, 6})
    {
        // In check only the two most recent plies are informative.
        if (ss->inCheck && i > 2)
            break;
        if ((ss - i)->currentMove.is_ok())
            (*(ss - i)->continuationHistory)[pc][to] << bonus / (1 + (i == 3));
    }
}

void Worker::update_quiet_histories(const Position& pos, Stack* ss, Move move, int bonus) {
    const Color us = pos.side_to_move();
    mainHistory[us][move.from_to()] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus);
    pawnHistory[pawn_history_index(pos)][pos.moved_piece(move)][move.to_sq()] << bonus / 2;
}

void Worker::update_all_stats(const Position& pos, Stack* ss, Move bestMove, Square prevSq,
                              const Move* quiets, int quietCount, const Move* captures,
                              int captureCount, Depth depth, bool isTTMove) {
    const int bonus = stat_bonus(depth) + 300 * isTTMove;
    const int malus = stat_malus(depth);

    auto capture_entry = [&](Move m) -> StatEntry<10692>& {
        const Piece     moved    = pos.moved_piece(m);
        const PieceType captured = m.type_of() == EN_PASSANT ? PAWN : type_of(pos.piece_on(m.to_sq()));
        return captureHistory[moved][m.to_sq()][captured];
    };

    if (!pos.capture_stage(bestMove))
    {
        update_quiet_histories(pos, ss, bestMove, bonus);

        if (ss->killers[0] != bestMove)
        {
            ss->killers[1] = ss->killers[0];
            ss->killers[0] = bestMove;
        }
        if (prevSq != SQ_NONE)
            counterMoves[pos.piece_on(prevSq)][prevSq] = bestMove;

        for (int i = 0; i < quietCount; ++i)
            update_quiet_histories(pos, ss, quiets[i], -malus);
    }
    else
        capture_entry(bestMove) << bonus;

    // The previous move was an early quiet move that got refuted.
    if (prevSq != SQ_NONE && (ss - 1)->moveCount == 1 + (ss - 1)->ttHit && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -malus);

    for (int i = 0; i < captureCount; ++i)
        capture_entry(captures[i]) << -malus;
}

// ---- time and reporting --------------------------------------------------------

void Worker::check_time() {
    if (++callsCnt < 512)
        return;
    callsCnt = 0;

    if (threads.ponder.load(std::memory_order_relaxed))
        return;

    const TimePoint elapsed = tm.elapsed();
    if ((limits.use_time_management() && (elapsed >= tm.maximum() || stopOnPonderhit))
        || (limits.movetime && elapsed >= limits.movetime)
        || (limits.nodes && threads.nodes_searched() >= limits.nodes))
        threads.stop = true;
}

void Worker::report(Depth depth) {
    // Called on the main worker, possibly to show another worker's result.
    const Worker&   main    = *threads.main_worker();
    const TimePoint elapsed = std::max<TimePoint>(1, main.tm.elapsed());
    const uint64_t  n       = threads.nodes_searched();
    const size_t    multiPV = std::min<size_t>(size_t(threads.settings.multiPV), rootMoves.size());

    for (size_t i = 0; i < multiPV; ++i)
    {
        const RootMove& rm      = rootMoves[i];
        const bool      updated = rm.score != -VALUE_INFINITE;
        if (depth == 1 && !updated && i > 0)
            continue;

        Info info;
        info.depth      = updated ? depth : std::max(1, depth - 1);
        info.selDepth   = rm.selDepth;
        info.multiPV    = int(i + 1);
        info.score      = updated ? rm.uciScore : rm.previousScore;
        if (info.score == -VALUE_INFINITE)
            info.score = VALUE_ZERO;
        info.lowerbound = updated && rm.lowerbound;
        info.upperbound = updated && rm.upperbound;
        info.nodes      = n;
        info.nps        = n * 1000 / uint64_t(elapsed);
        info.hashfull   = tt.hashfull();
        info.time       = elapsed;
        info.pv         = rm.pv;
        threads.callbacks.onInfo(info);
    }
}

void Worker::start_searching() {
    if (!is_main())
    {
        iterative_deepening();
        return;
    }

    tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), threads.settings.moveOverhead,
            threads.settings.ponder);
    tt.new_search();
    stopOnPonderhit = false;
    callsCnt        = 0;

    if (!rootMoves.empty())
    {
        threads.start_helpers();
        iterative_deepening();
    }

    // In ponder or infinite mode the search may finish early, but the best
    // move must not be sent before "stop" or "ponderhit" arrives.
    while (!threads.stop && (threads.ponder || limits.infinite))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    threads.stop = true;
    threads.wait_for_helpers();

    if (rootMoves.empty())
    {
        Info info{};
        info.score = rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW;
        info.multiPV = 1;
        threads.callbacks.onInfo(info);
        threads.callbacks.onBestMove(Move::none(), Move::none());
        return;
    }

    Worker* best = this;
    if (threads.settings.multiPV == 1 && !limits.depth && rootMoves[0].pv[0] != Move::none())
        best = threads.best_worker();

    bestPreviousScore        = best->rootMoves[0].score;
    bestPreviousAverageScore = best->rootMoves[0].averageScore;

    // Show the line of the thread whose move is played.
    if (best != this)
        best->report(best->completedDepth);

    const auto& pv = best->rootMoves[0].pv;
    threads.callbacks.onBestMove(pv[0], pv.size() > 1 ? pv[1] : Move::none());
}

// ---- iterative deepening ---------------------------------------------------------

void Worker::iterative_deepening() {
    Move  pv[MAX_PLY + 1];
    Stack stack[MAX_PLY + 10];
    std::memset(static_cast<void*>(stack), 0, sizeof(stack));
    Stack* ss = stack + 7;

    // Sentinels: (ss - 7) .. (ss - 1) are read by the history code.
    for (int i = 7; i > 0; --i)
    {
        (ss - i)->continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
        (ss - i)->continuationCorrectionHistory = &continuationCorrection[NO_PIECE][0];
        (ss - i)->staticEval                    = VALUE_NONE;
        (ss - i)->currentMove                   = Move::none();
    }
    for (int i = 0; i <= MAX_PLY + 2; ++i)
        (ss + i)->ply = i;
    ss->pv = pv;

    Value  bestValue          = -VALUE_INFINITE;
    Move   lastBestMove       = Move::none();
    Depth  lastBestMoveDepth  = 0;
    double timeReduction      = 1.0, totBestMoveChanges = 0;
    int    iterIdx            = 0;
    int    searchAgainCounter = 0;

    if (is_main())
        for (Value& v : iterValue)
            v = bestPreviousScore == VALUE_INFINITE ? VALUE_ZERO : bestPreviousScore;

    const size_t multiPV = std::min<size_t>(size_t(threads.settings.multiPV), rootMoves.size());
    const Color  us      = rootPos.side_to_move();

    accumulators.reset();
    nmpMinPly = 0;
    rootDepth = completedDepth = 0;

    while (++rootDepth < MAX_PLY && !threads.stop
           && !(limits.depth && is_main() && rootDepth > limits.depth))
    {
        if (is_main())
            totBestMoveChanges /= 2;

        for (RootMove& rm : rootMoves)
            rm.previousScore = rm.score;

        pvLast = rootMoves.size();

        if (!threads.increaseDepth)
            ++searchAgainCounter;

        for (pvIdx = 0; pvIdx < multiPV && !threads.stop; ++pvIdx)
        {
            selDepth = 0;

            // Aspiration window around the running average score.
            const Value avg   = rootMoves[pvIdx].averageScore;
            Value       delta = AspirationDelta + int(std::min<int64_t>(int64_t(avg) * avg / AspirationDiv, 2 * VALUE_INFINITE));
            Value       alpha = std::max(avg - delta, -VALUE_INFINITE);
            Value       beta  = std::min(avg + delta, VALUE_INFINITE);

            optimism[us]  = avg == -VALUE_INFINITE ? 0 : OptimismMul * avg / (std::abs(avg) + OptimismDiv);
            optimism[~us] = -optimism[us];

            int failedHighCnt = 0;
            while (true)
            {
                const Depth adjustedDepth =
                  std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);
                rootDelta = beta - alpha;
                bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);

                // Keep the already searched lines in order even if stopped.
                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

                if (threads.stop)
                    break;

                if (is_main() && multiPV == 1 && (bestValue <= alpha || bestValue >= beta)
                    && tm.elapsed() > 3000)
                    report(rootDepth);

                if (bestValue <= alpha)
                {
                    beta          = (alpha + beta) / 2;
                    alpha         = std::max(bestValue - delta, -VALUE_INFINITE);
                    failedHighCnt = 0;
                    if (is_main())
                        stopOnPonderhit = false;
                }
                else if (bestValue >= beta)
                {
                    beta = std::min(bestValue + delta, VALUE_INFINITE);
                    ++failedHighCnt;
                }
                else
                    break;

                delta += delta / 3;
            }

            std::stable_sort(rootMoves.begin(), rootMoves.begin() + pvIdx + 1);

            if (is_main() && !threads.stop && (pvIdx + 1 == multiPV || tm.elapsed() > 3000))
                report(rootDepth);
        }

        if (!threads.stop)
            completedDepth = rootDepth;

        if (rootMoves[0].pv[0] != lastBestMove)
        {
            lastBestMove      = rootMoves[0].pv[0];
            lastBestMoveDepth = rootDepth;
        }

        if (!is_main())
            continue;

        // "go mate N": stop once a short enough mate is found.
        if (limits.mate && rootMoves[0].score == rootMoves[0].uciScore
            && ((rootMoves[0].score >= VALUE_MATE_IN_MAX_PLY
                 && VALUE_MATE - rootMoves[0].score <= 2 * limits.mate)
                || (rootMoves[0].score != -VALUE_INFINITE
                    && rootMoves[0].score <= VALUE_MATED_IN_MAX_PLY
                    && VALUE_MATE + rootMoves[0].score <= 2 * limits.mate)))
            threads.stop = true;

        totBestMoveChanges += double(threads.collect_best_move_changes());

        if (limits.use_time_management() && !threads.stop && !stopOnPonderhit)
        {
            const uint64_t totalNodes  = std::max<uint64_t>(1, threads.nodes_searched());
            const uint64_t nodesEffort = rootMoves[0].effort * 100000 / totalNodes;

            // Spend more when the score is dropping, less when it is stable.
            const double fallingEval =
              std::clamp((11.4 + 2.1 * (bestPreviousAverageScore == VALUE_INFINITE ? 0 : bestPreviousAverageScore - bestValue)
                          + 1.0 * (iterValue[iterIdx] - bestValue))
                           / 100.0,
                         0.58, 1.67);

            // Less when the best move has not changed for a while.
            timeReduction            = lastBestMoveDepth + 8 < completedDepth ? 1.50 : 0.69;
            const double reduction   = (1.48 + previousTimeReduction) / (2.17 * timeReduction);
            const double instability = 1.0 + 1.88 * totBestMoveChanges / double(threads.size());

            double totalTime = double(tm.optimum()) * fallingEval * reduction * instability;

            // Only one legal move: no point in thinking long.
            if (rootMoves.size() == 1)
                totalTime = std::min(500.0, totalTime);

            const double elapsed = double(tm.elapsed());

            if (completedDepth >= 10 && nodesEffort >= 97000 && elapsed > totalTime * 0.74
                && !threads.ponder)
                threads.stop = true;

            if (elapsed > totalTime)
            {
                // While pondering we may not stop on our own; remember to
                // stop as soon as the opponent plays the expected move.
                if (threads.ponder)
                    stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else
                threads.increaseDepth = threads.ponder || elapsed <= totalTime * 0.50;
        }

        iterValue[iterIdx] = bestValue;
        iterIdx            = (iterIdx + 1) & 3;
    }

    if (is_main())
        previousTimeReduction = timeReduction;
}

// ---- main search -------------------------------------------------------------------

template<NodeType NT>
Value Worker::search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {
    constexpr bool PvNode   = NT != NonPV;
    constexpr bool rootNode = NT == Root;
    const bool     allNode  = !(PvNode || cutNode);

    // Horizon: switch to quiescence search.
    if (depth <= 0)
        return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    depth = std::min(depth, MAX_PLY - 1);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || alpha == beta - 1);

    Move      pv[MAX_PLY + 1];
    StateInfo st;
    Move      quietsSearched[32], capturesSearched[32];
    int       quietCount = 0, captureCount = 0;

    const bool  inCheck = pos.checkers();
    const Color us      = pos.side_to_move();
    ss->inCheck         = inCheck;
    ss->moveCount       = 0;

    Value bestValue = -VALUE_INFINITE;
    Move  bestMove  = Move::none();
    Value value     = -VALUE_INFINITE;

    if (is_main())
        check_time();

    if (PvNode && selDepth < ss->ply + 1)
        selDepth = ss->ply + 1;

    if (!rootNode)
    {
        if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !inCheck) ? evaluate(pos)
                                                    : value_draw(nodes.load(std::memory_order_relaxed));

        // Mate distance pruning: a shorter mate was already found.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta  = std::min(mate_in(ss->ply + 1), beta);
        if (alpha >= beta)
            return alpha;
    }

    (ss + 1)->excludedMove = Move::none();
    (ss + 2)->killers[0] = (ss + 2)->killers[1] = Move::none();
    (ss + 2)->cutoffCnt                         = 0;
    ss->statScore                               = 0;

    const Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;

    // ---- transposition table ----
    const Move excludedMove = ss->excludedMove;
    const Key  posKey       = pos.key();
    bool       ttHit;
    TTData     ttData;
    TTEntry*   tte = tt.probe(posKey, ttHit, ttData);

    ss->ttHit    = ttHit;
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;
    if (rootNode)
        ttData.move = rootMoves[pvIdx].pv[0];
    else if (!ttHit || !pos.pseudo_legal(ttData.move) || !pos.legal(ttData.move))
        ttData.move = Move::none();  // a hash collision or torn entry: ignore the move

    if (!excludedMove)
        ss->ttPv = PvNode || (ttHit && ttData.isPv);

    const bool ttCapture = ttData.move != Move::none() && pos.capture_stage(ttData.move);

    // Cutoff from the table. Not at PV nodes, to keep full principal variations.
    if (!PvNode && !excludedMove && ttData.value != VALUE_NONE
        && ttData.depth > depth - (ttData.value <= beta)
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
        && (cutNode == (ttData.value >= beta) || depth > 5))
    {
        if (ttData.move != Move::none() && ttData.value >= beta)
        {
            if (!ttCapture)
                update_quiet_histories(pos, ss, ttData.move, stat_bonus(depth) * 3 / 4);
            if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !pos.captured_piece())
                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -stat_malus(depth + 1));
        }
        // Near the 50-move limit a stored score may no longer be reachable.
        if (pos.rule50_count() < 90)
            return ttData.value;
    }

    // ---- static evaluation ----
    Value     unadjustedStaticEval = VALUE_NONE;
    Value     eval                 = VALUE_NONE;
    bool      improving            = false;
    const int correction           = correction_value(pos, ss);

    if (inCheck)
        ss->staticEval = eval = (ss - 2)->staticEval;
    else
    {
        if (excludedMove)
            unadjustedStaticEval = eval = ss->staticEval;
        else if (ttHit)
        {
            unadjustedStaticEval = ttData.eval;
            if (unadjustedStaticEval == VALUE_NONE)
                unadjustedStaticEval = evaluate(pos);
            ss->staticEval = eval = to_corrected(unadjustedStaticEval, correction);

            // A bounded search result is a better estimate than the eval.
            if (ttData.value != VALUE_NONE
                && (ttData.bound & (ttData.value > eval ? BOUND_LOWER : BOUND_UPPER)))
                eval = ttData.value;
        }
        else
        {
            unadjustedStaticEval = evaluate(pos);
            ss->staticEval = eval = to_corrected(unadjustedStaticEval, correction);
            tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(),
                      unadjustedStaticEval, tt.generation());
        }

        // Use the eval swing caused by the previous quiet move to adjust
        // that move's history.
        if ((ss - 1)->currentMove.is_ok() && !(ss - 1)->inCheck && !pos.captured_piece()
            && (ss - 1)->staticEval != VALUE_NONE)
        {
            const int bonus = std::clamp(-10 * int((ss - 1)->staticEval + ss->staticEval), -1831, 1428) + 623;
            mainHistory[~us][(ss - 1)->currentMove.from_to()] << bonus;
            if (type_of(pos.piece_on(prevSq)) != PAWN && (ss - 1)->currentMove.type_of() != PROMOTION)
                pawnHistory[pawn_history_index(pos)][pos.piece_on(prevSq)][prevSq] << bonus / 2;
        }

        // Is our position getting better compared with two plies ago?
        improving                    = ss->staticEval > (ss - 2)->staticEval;
        const bool opponentWorsening = (ss - 1)->staticEval != VALUE_NONE
                                    && ss->staticEval + (ss - 1)->staticEval > 2;

        // Razoring: hopeless static eval, verify with quiescence search.
        if (!PvNode && eval < alpha - RazorBase - RazorDepth2 * depth * depth)
        {
            value = qsearch<NonPV>(pos, ss, alpha - 1, alpha);
            if (value < alpha && !is_win(value) && !is_loss(value))
                return value;
        }

        // Reverse futility pruning: eval is far above beta.
        if (!ss->ttPv && depth < RfpDepth
            && eval - futility_margin(depth, cutNode && !ttHit, improving, opponentWorsening)
                   - (ss - 1)->statScore / RfpStatDiv
                 >= beta
            && eval >= beta && (ttData.move == Move::none() || ttCapture) && !is_loss(beta)
            && !is_win(eval))
            return beta + (eval - beta) / 3;

        // Null move pruning: give the opponent a free move; if we still
        // beat beta the position is good enough to cut.
        if (cutNode && (ss - 1)->currentMove != Move::null() && eval >= beta
            && ss->staticEval >= beta - NmpEvalDepth * depth + NmpEvalBase && !excludedMove
            && pos.non_pawn_material(us) && ss->ply >= nmpMinPly && !is_loss(beta))
        {
            const Depth R = std::min((eval - beta) / NmpEvalDiv, 7) + depth / 3 + NmpBaseR;

            ss->currentMove                   = Move::null();
            ss->continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
            ss->continuationCorrectionHistory = &continuationCorrection[NO_PIECE][0];

            DirtyPiece dp;
            dp.count = 0;
            pos.do_null_move(st);
            accumulators.push(dp);
            const Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, false);
            accumulators.pop();
            pos.undo_null_move();

            if (nullValue >= beta && !is_win(nullValue))
            {
                if (nmpMinPly || depth < 16)
                    return nullValue;

                // High depth: verify without null moves for a while, to
                // catch zugzwang.
                nmpMinPly     = ss->ply + 3 * (depth - R) / 4;
                const Value v = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
                nmpMinPly     = 0;
                if (v >= beta)
                    return nullValue;
            }
        }

        improving |= ss->staticEval >= beta + 97;

        // Internal iterative reductions: without a table move the node is
        // likely unimportant or will be searched again with one.
        if (PvNode && ttData.move == Move::none())
            depth -= 3;
        if (depth <= 0)
            return qsearch<PV>(pos, ss, alpha, beta);
        if (cutNode && depth >= 7 && (ttData.move == Move::none() || ttData.bound == BOUND_UPPER))
            depth -= 2;

        // ProbCut: a good capture whose reduced search beats beta by a
        // margin almost always holds at full depth.
        const Value probCutBeta = beta + ProbCutMargin - ProbCutImproving * improving;
        if (!PvNode && depth > 3 && !is_win(beta) && !is_loss(beta)
            && !(ttData.depth >= depth - 3 && ttData.value != VALUE_NONE && ttData.value < probCutBeta))
        {
            MovePicker  mp(pos, ttData.move, probCutBeta - ss->staticEval, &captureHistory);
            const Depth probCutDepth = std::max(depth - 4, 0);
            Move        move;

            while ((move = mp.next_move()) != Move::none())
            {
                if (move == excludedMove)
                    continue;

                do_move(pos, move, st, pos.gives_check(move), ss);
                value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);
                if (value >= probCutBeta && probCutDepth > 0)
                    value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, probCutDepth, !cutNode);
                undo_move(pos, move);

                if (threads.stop.load(std::memory_order_relaxed))
                    return VALUE_ZERO;

                if (value >= probCutBeta)
                {
                    tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER,
                              probCutDepth + 1, move, unadjustedStaticEval, tt.generation());
                    if (!is_win(value) && !is_loss(value))
                        return value - (probCutBeta - beta);
                }
            }
        }
    }

    // A table entry that is already far above beta, from a not much
    // shallower search.
    if (!PvNode && !excludedMove && (ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 4
        && ttData.value != VALUE_NONE && ttData.value >= beta + ProbCutTTMargin && !is_win(beta)
        && !is_loss(beta) && !is_win(ttData.value))
        return beta + ProbCutTTMargin;

    // ---- move loop ----
    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory, (ss - 2)->continuationHistory,
                                        (ss - 3)->continuationHistory, (ss - 4)->continuationHistory,
                                        (ss - 5)->continuationHistory, (ss - 6)->continuationHistory};

    const Move counterMove = prevSq != SQ_NONE ? counterMoves[pos.piece_on(prevSq)][prevSq] : Move::none();

    MovePicker mp(pos, ttData.move, depth, &mainHistory, &captureHistory, contHist, &pawnHistory,
                  ss->killers, counterMove);

    int  moveCount         = 0;
    bool moveCountPruning  = false;
    Move move;

    while ((move = mp.next_move()) != Move::none())
    {
        if (move == excludedMove)
            continue;

        // At the root, only moves of the current multi-PV slice.
        if (rootNode && !std::count(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast, move))
            continue;

        ss->moveCount = ++moveCount;

        if (rootNode && is_main() && tm.elapsed() > 3000 && threads.callbacks.onCurrMove)
            threads.callbacks.onCurrMove(depth, move, moveCount + int(pvIdx));

        if (PvNode)
            (ss + 1)->pv = nullptr;

        const bool  capture    = pos.capture_stage(move);
        const Piece movedPiece = pos.moved_piece(move);
        const bool  givesCheck = pos.gives_check(move);

        Depth extension = 0;
        Depth newDepth  = depth - 1;

        const int delta = beta - alpha;
        int       r     = reduction(improving, depth, moveCount, delta);

        // ---- pruning at shallow depth ----
        if (!rootNode && pos.non_pawn_material(us) && !is_loss(bestValue))
        {
            // Late move pruning: enough moves tried, skip the remaining quiets.
            if (!moveCountPruning && moveCount >= futility_move_count(improving, depth))
            {
                moveCountPruning = true;
                mp.skip_quiet_moves();
            }

            int lmrDepth = newDepth - r / 1024;

            if (capture || givesCheck)
            {
                const Piece captured  = move.type_of() == EN_PASSANT ? make_piece(~us, PAWN) : pos.piece_on(move.to_sq());
                const int   captHist  = captureHistory[movedPiece][move.to_sq()][type_of(captured)];

                // Futility pruning for captures.
                if (!givesCheck && lmrDepth < 7 && !inCheck)
                {
                    const Value futilityValue = ss->staticEval + FutCaptBase + FutCaptDepth * lmrDepth
                                              + PieceValue[captured] + captHist / 7;
                    if (futilityValue <= alpha)
                        continue;
                }

                // Captures that lose material.
                const int seeHist = std::clamp(captHist / 32, -159 * depth, 160 * depth);
                if (!pos.see_ge(move, -SeeCaptDepth * depth - seeHist))
                    continue;
            }
            else
            {
                int history = (*contHist[0])[movedPiece][move.to_sq()] + (*contHist[1])[movedPiece][move.to_sq()]
                            + pawnHistory[pawn_history_index(pos)][movedPiece][move.to_sq()];

                // Quiet moves with a bad record.
                if (history < -HistPruneDepth * depth)
                    continue;

                history += 2 * mainHistory[us][move.from_to()];
                lmrDepth += history / HistLmrDiv;

                const Value futilityValue = ss->staticEval + (bestMove != Move::none() ? FutQuietBest : FutQuietBase)
                                          + FutQuietDepth * lmrDepth;

                // Futility pruning: static eval too far below alpha.
                if (!inCheck && lmrDepth < 12 && futilityValue <= alpha)
                {
                    if (bestValue <= futilityValue && !is_win(bestValue) && !is_loss(futilityValue))
                        bestValue = futilityValue;
                    continue;
                }

                lmrDepth = std::max(lmrDepth, 0);

                // Quiet moves that lose material.
                if (!pos.see_ge(move, -SeeQuietDepth2 * lmrDepth * lmrDepth))
                    continue;
            }
        }

        // ---- extensions ----
        // Singular extension: if every other move fails well below the
        // table move's score, that move is special and gets more depth.
        if (!rootNode && move == ttData.move && !excludedMove
            && depth >= 4 - (completedDepth > 33) + ss->ttPv && ttData.value != VALUE_NONE
            && !is_win(ttData.value) && !is_loss(ttData.value) && (ttData.bound & BOUND_LOWER)
            && ttData.depth >= depth - 3 && ss->ply < 2 * rootDepth)
        {
            const Value singularBeta  = ttData.value - (SingularMargin + SingularMarginTtPv * (ss->ttPv && !PvNode)) * depth / 64;
            const Depth singularDepth = newDepth / 2;

            ss->excludedMove = move;
            value            = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = Move::none();

            if (value < singularBeta)
            {
                const int doubleMargin = DoubleExtPv * PvNode - DoubleExtQuiet * !ttCapture;
                const int tripleMargin = TripleExtBase + TripleExtPv * PvNode - TripleExtQuiet * !ttCapture
                                       + TripleExtTtPv * ss->ttPv;

                extension = 1 + (value < singularBeta - doubleMargin) + (value < singularBeta - tripleMargin);
                depth += (!PvNode && depth < 14);
            }
            // Multi-cut: even without the table move we beat beta.
            else if (value >= beta && !is_win(value) && !is_loss(value))
                return value;
            // The table move is not singular; reduce instead where the node
            // is expected to fail high or low anyway.
            else if (ttData.value >= beta)
                extension = -3;
            else if (cutNode)
                extension = -2;
            else if (ttData.value <= value)
                extension = -1;
        }

        newDepth += extension;

        const uint64_t nodeCount = rootNode ? nodes.load(std::memory_order_relaxed) : 0;

        do_move(pos, move, st, givesCheck, ss);

        // ---- late move reductions ----
        if (ss->ttPv)
            r -= LmrTtPv + (ttData.value != VALUE_NONE && ttData.value > alpha) * LmrTtPvValue
               + (ttData.depth >= depth) * LmrTtPvDepth;
        if (PvNode)
            r -= LmrPvNode;
        if (cutNode)
            r += LmrCutNode - (ttData.depth >= depth && ss->ttPv) * LmrCutNodeTt;
        if (ttCapture && !capture)
            r += LmrTtCapture + (depth < 8) * 1024;
        if ((ss + 1)->cutoffCnt > 3)
            r += LmrCutoffCnt + allNode * 1024;
        else if (move == ttData.move)
            r -= LmrTtMove;
        if (givesCheck)
            r -= LmrCheck;

        if (capture)
        {
            const Piece captured = pos.captured_piece();
            ss->statScore = 7 * PieceValue[captured]
                          + captureHistory[movedPiece][move.to_sq()][type_of(captured)] - LmrCaptHistOffset;
        }
        else
            ss->statScore = 2 * mainHistory[us][move.from_to()] + (*contHist[0])[movedPiece][move.to_sq()]
                          + (*contHist[1])[movedPiece][move.to_sq()] - LmrQuietHistOffset;

        r -= ss->statScore * 1024 / LmrHistDiv;

        if (depth >= 2 && moveCount > 1)
        {
            // Reduced null-window search; extend a little where the move
            // ordering says the move is good.
            const Depth d = std::max(1, std::min(newDepth - r / 1024, newDepth + !allNode + (PvNode && bestMove == Move::none())));

            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);

            if (value > alpha && d < newDepth)
            {
                // The reduced search surprised us: search again at full
                // depth, slightly adjusted by how big the surprise was.
                const bool doDeeper    = value > bestValue + 42 + 2 * newDepth;
                const bool doShallower = value < bestValue + 10;
                newDepth += doDeeper - doShallower;

                if (newDepth > d)
                    value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                const int bonus = value >= beta ? stat_bonus(newDepth) * 2 : -stat_malus(newDepth);
                if (!capture)
                    update_continuation_histories(ss, movedPiece, move.to_sq(), bonus / 2);
            }
        }
        else if (!PvNode || moveCount > 1)
        {
            // Full-depth null-window search.
            if (ttData.move == Move::none())
                r += 2111;
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha,
                                   newDepth - (r > 3444) - (r > 5588 && newDepth > 2), !cutNode);
        }

        // Full window for the first move and for moves that beat alpha.
        if (PvNode && (moveCount == 1 || value > alpha))
        {
            (ss + 1)->pv    = pv;
            (ss + 1)->pv[0] = Move::none();
            value           = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        undo_move(pos, move);

        // A stopped search returns garbage: do not use it.
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);

            rm.effort += nodes.load(std::memory_order_relaxed) - nodeCount;
            rm.averageScore = rm.averageScore != -VALUE_INFINITE ? (value + rm.averageScore) / 2 : value;

            if (moveCount == 1 || value > alpha)
            {
                rm.score = rm.uciScore = value;
                rm.selDepth            = selDepth;
                rm.lowerbound = rm.upperbound = false;
                if (value >= beta)
                {
                    rm.lowerbound = true;
                    rm.uciScore   = beta;
                }
                else if (value <= alpha)
                {
                    rm.upperbound = true;
                    rm.uciScore   = alpha;
                }

                rm.pv.resize(1);
                for (const Move* m = (ss + 1)->pv; m && *m != Move::none(); ++m)
                    rm.pv.push_back(*m);

                // Best-move changes are a sign of an unstable position.
                if (moveCount > 1 && !pvIdx)
                    bestMoveChanges.store(bestMoveChanges.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            }
            else
                // Not a new best move: sink below the already sorted ones.
                rm.score = -VALUE_INFINITE;
        }

        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = move;

                if (PvNode && !rootNode)
                    update_pv(ss->pv, move, (ss + 1)->pv);

                if (value >= beta)
                {
                    ss->cutoffCnt += !ttData.move.is_ok() + (extension < 2);
                    break;
                }

                // A new best move: later moves are less likely to matter.
                if (depth > 2 && depth < 14 && !is_win(value) && !is_loss(value))
                    depth -= 2;

                alpha = value;
            }
        }

        // Remember what was tried, to penalise it if another move cuts.
        if (move != bestMove && moveCount <= 32)
        {
            if (capture)
                capturesSearched[captureCount++] = move;
            else
                quietsSearched[quietCount++] = move;
        }
    }

    // ---- no legal move, statistics, table store ----
    if (!moveCount)
        bestValue = excludedMove ? alpha : inCheck ? mated_in(ss->ply) : VALUE_DRAW;

    else if (bestMove != Move::none())
        update_all_stats(pos, ss, bestMove, prevSq, quietsSearched, quietCount, capturesSearched,
                         captureCount, depth, bestMove == ttData.move);

    // All moves failed low: the previous quiet move was good.
    else if (prevSq != SQ_NONE && !pos.captured_piece())
    {
        int bonusScale = 118 * (depth > 5) + 37 * !allNode + 169 * ((ss - 1)->moveCount > 8)
                       + 128 * (!inCheck && bestValue <= ss->staticEval - 102)
                       + 115 * (!(ss - 1)->inCheck && (ss - 1)->staticEval != VALUE_NONE && bestValue <= -(ss - 1)->staticEval - 82);
        bonusScale = std::max(bonusScale, 0);
        const int scaled = stat_bonus(depth) * bonusScale;

        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, scaled * 416 / 32768);
        mainHistory[~us][(ss - 1)->currentMove.from_to()] << scaled * 212 / 32768;
        if (type_of(pos.piece_on(prevSq)) != PAWN && (ss - 1)->currentMove.type_of() != PROMOTION)
            pawnHistory[pawn_history_index(pos)][pos.piece_on(prevSq)][prevSq] << scaled * 1073 / 32768;
    }

    // A fail-low below a PV node of the parent keeps the PV flag.
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss - 1)->ttPv && depth > 3);

    if (!excludedMove && !(rootNode && pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta                       ? BOUND_LOWER
                  : PvNode && bestMove != Move::none() ? BOUND_EXACT
                                                           : BOUND_UPPER,
                  depth, bestMove, unadjustedStaticEval, tt.generation());

    // Learn how far the static eval was off, where the search result is a
    // usable comparison (quiet best move, bound on the right side).
    if (!inCheck && !excludedMove && !(bestMove != Move::none() && pos.capture(bestMove))
        && ((bestValue < ss->staticEval && bestValue < beta)
            || (bestValue > ss->staticEval && bestMove != Move::none())))
    {
        const int bonus = std::clamp(int(bestValue - ss->staticEval) * depth / 8, -CorrectionLimit / 4, CorrectionLimit / 4);
        update_correction(pos, ss, bonus);
    }

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);
    return bestValue;
}

// ---- quiescence search ----------------------------------------------------------------

template<NodeType NT>
Value Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {
    constexpr bool PvNode = NT == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);

    Move      pv[MAX_PLY + 1];
    StateInfo st;

    if (PvNode)
    {
        (ss + 1)->pv = pv;
        ss->pv[0]    = Move::none();
    }

    const bool inCheck = pos.checkers();
    ss->inCheck        = inCheck;
    Move  bestMove     = Move::none();
    int   moveCount    = 0;

    if (PvNode && selDepth < ss->ply + 1)
        selDepth = ss->ply + 1;

    if (pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !inCheck) ? evaluate(pos) : VALUE_DRAW;

    // ---- transposition table ----
    const Key posKey = pos.key();
    bool      ttHit;
    TTData    ttData;
    TTEntry*  tte = tt.probe(posKey, ttHit, ttData);

    ss->ttHit    = ttHit;
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;
    if (!ttHit)
        ttData.move = Move::none();
    const bool pvHit = ttHit && ttData.isPv;

    if (!PvNode && ttData.depth >= DEPTH_QS && ttData.value != VALUE_NONE
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttData.value;

    // ---- stand pat ----
    Value unadjustedStaticEval = VALUE_NONE;
    Value bestValue, futilityBase;

    if (inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else
    {
        const int correction = correction_value(pos, ss);

        if (ttHit)
        {
            unadjustedStaticEval = ttData.eval;
            if (unadjustedStaticEval == VALUE_NONE)
                unadjustedStaticEval = evaluate(pos);
            ss->staticEval = bestValue = to_corrected(unadjustedStaticEval, correction);

            if (ttData.value != VALUE_NONE && !is_win(ttData.value) && !is_loss(ttData.value)
                && (ttData.bound & (ttData.value > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttData.value;
        }
        else
        {
            unadjustedStaticEval = evaluate(pos);
            ss->staticEval = bestValue = to_corrected(unadjustedStaticEval, correction);
        }

        // Standing pat is already good enough.
        if (bestValue >= beta)
        {
            if (!is_win(bestValue) && !is_loss(bestValue))
                bestValue = (bestValue + beta) / 2;
            if (!ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER, DEPTH_UNSEARCHED,
                          Move::none(), unadjustedStaticEval, tt.generation());
            return bestValue;
        }

        if (bestValue > alpha)
            alpha = bestValue;

        futilityBase = ss->staticEval + QsFutility;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory, (ss - 2)->continuationHistory,
                                        (ss - 3)->continuationHistory, (ss - 4)->continuationHistory,
                                        (ss - 5)->continuationHistory, (ss - 6)->continuationHistory};

    const Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;

    // Captures and queen promotions; all evasions when in check.
    MovePicker mp(pos, ttData.move, DEPTH_QS, &mainHistory, &captureHistory, contHist, &pawnHistory,
                  nullptr, Move::none());
    Move  move;
    Value value;

    while ((move = mp.next_move()) != Move::none())
    {
        const bool givesCheck = pos.gives_check(move);
        const bool capture    = pos.capture_stage(move);

        ++moveCount;

        if (!is_loss(bestValue))
        {
            // Futility pruning and move count pruning.
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase)
                && move.type_of() != PROMOTION)
            {
                if (moveCount > 2)
                    continue;

                const Value futilityValue = futilityBase + PieceValue[pos.piece_on(move.to_sq())];

                // Even winning the piece does not reach alpha.
                if (futilityValue <= alpha)
                {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }

                // The exchange does not gain enough to reach alpha.
                if (!pos.see_ge(move, alpha - futilityBase))
                {
                    bestValue = std::min(alpha, futilityBase);
                    continue;
                }
            }

            // Quiet evasions with a bad record.
            if (!capture
                && (*contHist[0])[pos.moved_piece(move)][move.to_sq()]
                       + (*contHist[1])[pos.moved_piece(move)][move.to_sq()]
                       + pawnHistory[pawn_history_index(pos)][pos.moved_piece(move)][move.to_sq()]
                     <= 5389)
                continue;

            // Captures that lose material.
            if (!pos.see_ge(move, -QsSee))
                continue;
        }

        do_move(pos, move, st, givesCheck, ss);
        value = -qsearch<NT>(pos, ss + 1, -beta, -alpha);
        undo_move(pos, move);

        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = move;

                if (PvNode)
                    update_pv(ss->pv, move, (ss + 1)->pv);

                if (value < beta)
                    alpha = value;
                else
                    break;
            }
        }
    }

    // In check with no legal move: mate. (All evasions were generated.)
    if (inCheck && bestValue == -VALUE_INFINITE)
        return mated_in(ss->ply);

    if (!is_win(bestValue) && !is_loss(bestValue) && bestValue >= beta)
        bestValue = (3 * bestValue + beta) / 4;

    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, bestMove, unadjustedStaticEval,
              tt.generation());

    return bestValue;
}

}  // namespace Nest::Search
