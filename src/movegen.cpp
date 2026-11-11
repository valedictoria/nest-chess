/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "movegen.h"

#include "bitboard.h"
#include "position.h"

namespace Nest {

namespace {

template<GenType Type, bool Enemy>
inline ExtMove* make_promotions(ExtMove* list, Square from, Square to) {
    constexpr bool all = Type == EVASIONS || Type == LEGAL;

    if (Type == CAPTURES || all)
        *list++ = Move::make<PROMOTION>(from, to, QUEEN);

    if ((Type == CAPTURES && Enemy) || (Type == QUIETS && !Enemy) || all)
    {
        *list++ = Move::make<PROMOTION>(from, to, ROOK);
        *list++ = Move::make<PROMOTION>(from, to, BISHOP);
        *list++ = Move::make<PROMOTION>(from, to, KNIGHT);
    }
    return list;
}

// checkMask: squares a non-king move must land on (all squares when not in
// check, otherwise the checker and the squares between it and the king).
template<Color Us, GenType Type>
ExtMove* generate_pawn_moves(const Position& pos, ExtMove* list, Bitboard checkMask) {
    constexpr Color     Them     = ~Us;
    constexpr Bitboard  TRank7BB = (Us == WHITE ? Rank7BB : Rank2BB);
    constexpr Bitboard  TRank3BB = (Us == WHITE ? Rank3BB : Rank6BB);
    constexpr Direction Up       = pawn_push(Us);
    constexpr Direction UpRight  = (Us == WHITE ? NORTH_EAST : SOUTH_WEST);
    constexpr Direction UpLeft   = (Us == WHITE ? NORTH_WEST : SOUTH_EAST);

    const Square   ksq     = pos.king_square(Us);
    const Bitboard pawns   = pos.pieces(Us, PAWN);
    const Bitboard pinned  = pos.blockers_for_king(Us) & pawns;
    const Bitboard free    = pawns & ~pinned;
    const Bitboard empty   = ~pos.pieces();
    const Bitboard enemies = pos.pieces(Them) & checkMask;

    // Pushes. A pinned pawn may push only along the king's file.
    if (Type != CAPTURES)
    {
        const Bitboard pushers = (free | (pinned & file_bb(ksq))) & ~TRank7BB;
        Bitboard       b1      = shift<Up>(pushers) & empty;
        Bitboard       b2      = shift<Up>(b1 & TRank3BB) & empty & checkMask;
        b1 &= checkMask;

        while (b1)
        {
            const Square to = pop_lsb(b1);
            *list++         = Move(to - Up, to);
        }
        while (b2)
        {
            const Square to = pop_lsb(b2);
            *list++         = Move(to - Up - Up, to);
        }
    }

    // Promotions by unpinned pawns (a pinned pawn can only promote by
    // capturing along the pin, handled below).
    if (const Bitboard on7 = free & TRank7BB)
    {
        Bitboard b1 = shift<UpRight>(on7) & enemies;
        Bitboard b2 = shift<UpLeft>(on7) & enemies;
        Bitboard b3 = shift<Up>(on7) & empty & checkMask;

        while (b1)
        {
            const Square to = pop_lsb(b1);
            list            = make_promotions<Type, true>(list, to - UpRight, to);
        }
        while (b2)
        {
            const Square to = pop_lsb(b2);
            list            = make_promotions<Type, true>(list, to - UpLeft, to);
        }
        while (b3)
        {
            const Square to = pop_lsb(b3);
            list            = make_promotions<Type, false>(list, to - Up, to);
        }
    }

    if (Type != QUIETS)
    {
        const Bitboard notOn7 = free & ~TRank7BB;
        Bitboard       b1     = shift<UpRight>(notOn7) & enemies;
        Bitboard       b2     = shift<UpLeft>(notOn7) & enemies;

        while (b1)
        {
            const Square to = pop_lsb(b1);
            *list++         = Move(to - UpRight, to);
        }
        while (b2)
        {
            const Square to = pop_lsb(b2);
            *list++         = Move(to - UpLeft, to);
        }

        if (pos.ep_square() != SQ_NONE)
        {
            const Square ep    = pos.ep_square();
            const Square capsq = ep - Up;

            // In check, en passant must capture the checker or block.
            if (checkMask & (square_bb(ep) | square_bb(capsq)))
            {
                Bitboard b = pawns & pawn_attacks_bb(Them, ep);
                while (b)
                {
                    const Square from = pop_lsb(b);
                    // Two pawns leave the rank at once: test the king directly.
                    const Bitboard occ =
                      (pos.pieces() ^ square_bb(from) ^ square_bb(capsq)) | square_bb(ep);
                    if (!(attacks_bb<ROOK>(ksq, occ) & pos.pieces(Them, ROOK, QUEEN))
                        && !(attacks_bb<BISHOP>(ksq, occ) & pos.pieces(Them, BISHOP, QUEEN)))
                        *list++ = Move::make<EN_PASSANT>(from, ep);
                }
            }
        }
    }

    // Captures by pinned pawns: only along the pin line.
    if (pinned && Type != QUIETS)
    {
        Bitboard pp = pinned;
        while (pp)
        {
            const Square from = pop_lsb(pp);
            Bitboard     b    = pawn_attacks_bb(Us, from) & enemies & line_bb(ksq, from);
            while (b)
            {
                const Square to = pop_lsb(b);
                if (square_bb(from) & TRank7BB)
                    list = make_promotions<Type, true>(list, from, to);
                else
                    *list++ = Move(from, to);
            }
        }
    }

    return list;
}

template<Color Us, GenType Type>
ExtMove* generate_all(const Position& pos, ExtMove* list) {
    constexpr Color Them = ~Us;

    const Square   ksq      = pos.king_square(Us);
    const Bitboard checkers = pos.checkers();
    const Bitboard occ      = pos.pieces();
    const Bitboard typeMask = Type == CAPTURES ? pos.pieces(Them)
                            : Type == QUIETS   ? ~occ
                                               : ~pos.pieces(Us);

    // In double check only the king can move.
    if (!more_than_one(checkers))
    {
        const Bitboard checkMask = checkers ? between_bb(ksq, lsb(checkers)) : ~Bitboard(0);
        const Bitboard target    = typeMask & checkMask;
        const Bitboard pinned    = pos.blockers_for_king(Us) & pos.pieces(Us);

        list = generate_pawn_moves<Us, Type>(pos, list, checkMask);

        // A pinned knight never has a legal move.
        Bitboard bb = pos.pieces(Us, KNIGHT) & ~pinned;
        while (bb)
        {
            const Square from = pop_lsb(bb);
            Bitboard     b    = attacks_bb<KNIGHT>(from) & target;
            while (b)
                *list++ = Move(from, pop_lsb(b));
        }

        for (PieceType pt : {BISHOP, ROOK, QUEEN})
        {
            bb = pos.pieces(Us, pt);
            while (bb)
            {
                const Square from = pop_lsb(bb);
                Bitboard     b    = attacks_bb(pt, from, occ) & target;
                if (pinned & square_bb(from))
                    b &= line_bb(ksq, from);
                while (b)
                    *list++ = Move(from, pop_lsb(b));
            }
        }

        if (Type != CAPTURES && !checkers && pos.can_castle(Us == WHITE ? WHITE_CASTLING : BLACK_CASTLING))
            for (CastlingRights cr : {Us == WHITE ? WHITE_OO : BLACK_OO, Us == WHITE ? WHITE_OOO : BLACK_OOO})
                if (pos.can_castle(cr) && !pos.castling_impeded(cr))
                {
                    const Square rsq = pos.castling_rook_square(cr);
                    const Square kto = relative_square(Us, rsq > ksq ? SQ_G1 : SQ_C1);

                    // No square on the king's way may be attacked.
                    bool     safe = true;
                    Bitboard path = between_bb(ksq, kto);
                    while (path && safe)
                        safe = !pos.attacked_by(Them, pop_lsb(path), occ);

                    // Chess960: the rook may be shielding the king's rank.
                    if (safe && !(pos.is_chess960() && (pos.blockers_for_king(Us) & square_bb(rsq))))
                        *list++ = Move::make<CASTLING>(ksq, rsq);
                }
    }

    // King moves: the king must not stay on an attacked line, so test with
    // the king lifted off the board.
    Bitboard       b         = attacks_bb<KING>(ksq) & typeMask;
    const Bitboard occNoKing = occ ^ square_bb(ksq);
    while (b)
    {
        const Square to = pop_lsb(b);
        if (!pos.attacked_by(Them, to, occNoKing))
            *list++ = Move(ksq, to);
    }

    return list;
}

}  // namespace

template<GenType Type>
ExtMove* generate(const Position& pos, ExtMove* list) {
    return pos.side_to_move() == WHITE ? generate_all<WHITE, Type>(pos, list)
                                       : generate_all<BLACK, Type>(pos, list);
}

template ExtMove* generate<CAPTURES>(const Position&, ExtMove*);
template ExtMove* generate<QUIETS>(const Position&, ExtMove*);
template ExtMove* generate<EVASIONS>(const Position&, ExtMove*);
template ExtMove* generate<LEGAL>(const Position&, ExtMove*);

}  // namespace Nest
