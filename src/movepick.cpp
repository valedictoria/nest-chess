/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "movepick.h"

#include <algorithm>

#include "bitboard.h"

namespace Nest {

namespace {

enum Stages {
    // main search
    MAIN_TT,
    CAPTURE_INIT,
    GOOD_CAPTURE,
    REFUTATION,
    QUIET_INIT,
    GOOD_QUIET,
    BAD_CAPTURE,
    BAD_QUIET,

    // in check
    EVASION_TT,
    EVASION_INIT,
    EVASION,

    // ProbCut
    PROBCUT_TT,
    PROBCUT_INIT,
    PROBCUT,

    // quiescence
    QSEARCH_TT,
    QCAPTURE_INIT,
    QCAPTURE
};

// Sorts in descending order the moves with value >= limit; the others keep
// their relative order behind them.
void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {
    for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
        if (p->value >= limit)
        {
            ExtMove tmp = *p, *q;
            *p          = *++sortedEnd;
            for (q = sortedEnd; q != begin && (q - 1)->value < tmp.value; --q)
                *q = *(q - 1);
            *q = tmp;
        }
}

constexpr int GoodQuietThreshold = -7998;

}  // namespace

MovePicker::MovePicker(const Position&         p,
                       Move                    ttm,
                       Depth                   d,
                       const ButterflyHistory* mh,
                       const CaptureHistory*   cph,
                       const PieceToHistory**  ch,
                       const PawnHistory*      ph,
                       const Move*             killers,
                       Move                    counterMove) :
    pos(p),
    mainHistory(mh),
    captureHistory(cph),
    continuationHistory(ch),
    pawnHistory(ph),
    ttMove(ttm),
    depth(d) {
    cur = endMoves = endBadCaptures = beginBadQuiets = endBadQuiets = moves;
    refutations[0] = killers ? killers[0] : Move::none();
    refutations[1] = killers ? killers[1] : Move::none();
    refutations[2] = counterMove;

    if (pos.checkers())
        stage = EVASION_TT;
    else
        stage = depth > 0 ? MAIN_TT : QSEARCH_TT;

    // The TT move is trusted only after a full legality check; in quiescence
    // (not in check) it must also be a capture-stage move.
    const bool usable = ttm != Move::none() && pos.pseudo_legal(ttm) && pos.legal(ttm)
                     && (depth > 0 || pos.checkers() || pos.capture_stage(ttm));
    if (!usable)
    {
        ttMove = Move::none();
        ++stage;
    }
}

MovePicker::MovePicker(const Position& p, Move ttm, int th, const CaptureHistory* cph) :
    pos(p),
    captureHistory(cph),
    ttMove(ttm),
    threshold(th) {
    cur = endMoves = endBadCaptures = beginBadQuiets = endBadQuiets = moves;
    refutations[0] = refutations[1] = refutations[2] = Move::none();
    stage = PROBCUT_TT;
    const bool usable = ttm != Move::none() && pos.capture_stage(ttm) && pos.pseudo_legal(ttm)
                     && pos.legal(ttm) && pos.see_ge(ttm, threshold);
    if (!usable)
    {
        ttMove = Move::none();
        ++stage;
    }
}

void MovePicker::score_captures() {
    for (ExtMove* m = cur; m != endMoves; ++m)
    {
        const Piece captured = m->move.type_of() == EN_PASSANT ? make_piece(~pos.side_to_move(), PAWN)
                                                               : pos.piece_on(m->move.to_sq());
        m->value = 7 * PieceValue[captured]
                 + (*captureHistory)[pos.moved_piece(m->move)][m->move.to_sq()][type_of(captured)];
    }
}

void MovePicker::score_quiets() {
    const Color us   = pos.side_to_move();
    const Color them = ~us;

    // Squares where a piece of the given class would be attacked by a
    // cheaper enemy piece.
    const Bitboard byPawn = us == WHITE ? pawn_attacks_bb<BLACK>(pos.pieces(them, PAWN))
                                        : pawn_attacks_bb<WHITE>(pos.pieces(them, PAWN));
    Bitboard byMinor = byPawn;
    for (Bitboard b = pos.pieces(them, KNIGHT); b;)
        byMinor |= attacks_bb<KNIGHT>(pop_lsb(b));
    for (Bitboard b = pos.pieces(them, BISHOP); b;)
        byMinor |= attacks_bb<BISHOP>(pop_lsb(b), pos.pieces());
    Bitboard byRook = byMinor;
    for (Bitboard b = pos.pieces(them, ROOK); b;)
        byRook |= attacks_bb<ROOK>(pop_lsb(b), pos.pieces());

    const Bitboard threatened = (pos.pieces(us, QUEEN) & byRook) | (pos.pieces(us, ROOK) & byMinor)
                              | (pos.pieces(us, KNIGHT, BISHOP) & byPawn);
    const int      pawnIdx    = pawn_history_index(pos);

    for (ExtMove* m = cur; m != endMoves; ++m)
    {
        const Piece     pc   = pos.moved_piece(m->move);
        const PieceType pt   = type_of(pc);
        const Square    from = m->move.from_sq();
        const Square    to   = m->move.to_sq();

        int v = 2 * (*mainHistory)[us][m->move.from_to()];
        v += 2 * (*pawnHistory)[pawnIdx][pc][to];
        v += (*continuationHistory[0])[pc][to];
        v += (*continuationHistory[1])[pc][to];
        v += (*continuationHistory[2])[pc][to] / 3;
        v += (*continuationHistory[3])[pc][to];
        v += (*continuationHistory[5])[pc][to];

        // Moves that give check.
        v += bool(pos.check_squares(pt) & square_bb(to)) * 16384;

        // Escaping an attack by a cheaper piece.
        if (threatened & square_bb(from))
            v += pt == QUEEN && !(square_bb(to) & byRook)   ? 51700
               : pt == ROOK && !(square_bb(to) & byMinor)   ? 25600
               : pt != QUEEN && pt != ROOK && !(square_bb(to) & byPawn) ? 14450
                                                             : 0;

        // Walking into one.
        v -= pt == QUEEN ? bool(square_bb(to) & byRook) * 49000
           : pt == ROOK  ? bool(square_bb(to) & byMinor) * 24335
           : pt != PAWN  ? bool(square_bb(to) & byPawn) * 14900
                         : 0;

        m->value = v;
    }
}

void MovePicker::score_evasions() {
    for (ExtMove* m = cur; m != endMoves; ++m)
    {
        if (pos.capture_stage(m->move))
            m->value = PieceValue[pos.piece_on(m->move.to_sq())] - int(type_of(pos.moved_piece(m->move)))
                     + (1 << 28);
        else
            m->value = (*mainHistory)[pos.side_to_move()][m->move.from_to()]
                     + (*continuationHistory[0])[pos.moved_piece(m->move)][m->move.to_sq()]
                     + (*pawnHistory)[pawn_history_index(pos)][pos.moved_piece(m->move)][m->move.to_sq()];
    }
}

// Returns the next move from [cur, endMoves) that is not the TT move and
// satisfies the predicate.
template<typename Pred>
Move MovePicker::select(Pred pred) {
    for (; cur < endMoves; ++cur)
        if (cur->move != ttMove && pred())
            return *cur++;
    return Move::none();
}

Move MovePicker::next_move() {
top:
    switch (stage)
    {
    case MAIN_TT :
    case EVASION_TT :
    case QSEARCH_TT :
    case PROBCUT_TT :
        ++stage;
        return ttMove;

    case CAPTURE_INIT :
    case PROBCUT_INIT :
    case QCAPTURE_INIT :
        cur = endBadCaptures = moves;
        endMoves             = generate<CAPTURES>(pos, cur);
        score_captures();
        partial_insertion_sort(cur, endMoves, -(1 << 30));
        ++stage;
        goto top;

    case GOOD_CAPTURE :
        if (select([&]() {
                // Losing captures are kept for later.
                if (pos.see_ge(cur->move, -cur->value / 18))
                    return true;
                *endBadCaptures++ = *cur;
                return false;
            }))
            return *(cur - 1);

        // Killers and counter move: only quiet moves not already tried.
        if (refutations[2] == refutations[0] || refutations[2] == refutations[1])
            refutations[2] = Move::none();
        if (refutations[1] == refutations[0])
            refutations[1] = Move::none();
        cur = moves;  // reused as an index into refutations[]
        ++stage;
        [[fallthrough]];

    case REFUTATION :
        while (!skipQuiets && cur < moves + 3)
        {
            const Move m = refutations[cur++ - moves];
            if (m != Move::none() && m != ttMove && !pos.capture_stage(m) && pos.pseudo_legal(m)
                && pos.legal(m))
                return m;
        }
        ++stage;
        [[fallthrough]];

    case QUIET_INIT :
        if (!skipQuiets)
        {
            cur      = endBadCaptures;
            endMoves = beginBadQuiets = endBadQuiets = generate<QUIETS>(pos, cur);
            score_quiets();
            partial_insertion_sort(cur, endMoves, -3560 * depth);
        }
        ++stage;
        [[fallthrough]];

    case GOOD_QUIET :
        if (!skipQuiets && select([&]() {
                return cur->move != refutations[0] && cur->move != refutations[1]
                    && cur->move != refutations[2];
            }))
        {
            if ((cur - 1)->value > GoodQuietThreshold || (cur - 1)->value <= -3560 * depth)
                return *(cur - 1);
            // The remaining quiets are poor: try losing captures first.
            beginBadQuiets = cur - 1;
        }

        cur      = moves;
        endMoves = endBadCaptures;
        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE :
        if (select([]() { return true; }))
            return *(cur - 1);

        cur      = beginBadQuiets;
        endMoves = endBadQuiets;
        ++stage;
        [[fallthrough]];

    case BAD_QUIET :
        if (!skipQuiets)
            return select([&]() {
                return cur->move != refutations[0] && cur->move != refutations[1]
                    && cur->move != refutations[2];
            });
        return Move::none();

    case EVASION_INIT :
        cur      = moves;
        endMoves = generate<EVASIONS>(pos, cur);
        score_evasions();
        partial_insertion_sort(cur, endMoves, -(1 << 30));
        ++stage;
        [[fallthrough]];

    case EVASION :
    case QCAPTURE :
        return select([]() { return true; });

    case PROBCUT :
        return select([&]() { return pos.see_ge(cur->move, threshold); });
    }

    return Move::none();
}

}  // namespace Nest
