/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <bit>
#include <string>

#include "types.h"

#if defined(USE_PEXT)
    #include <immintrin.h>
#endif

namespace Nest {

namespace Bitboards {
void        init();
std::string pretty(Bitboard b);
}

constexpr Bitboard FileABB = 0x0101010101010101ULL;
constexpr Bitboard FileHBB = FileABB << 7;
constexpr Bitboard Rank1BB = 0xFFULL;
constexpr Bitboard Rank2BB = Rank1BB << 8;
constexpr Bitboard Rank3BB = Rank1BB << 16;
constexpr Bitboard Rank6BB = Rank1BB << 40;
constexpr Bitboard Rank7BB = Rank1BB << 48;
constexpr Bitboard Rank8BB = Rank1BB << 56;

constexpr Bitboard square_bb(Square s) { return 1ULL << s; }
constexpr Bitboard rank_bb(Rank r) { return Rank1BB << (8 * r); }
constexpr Bitboard rank_bb(Square s) { return rank_bb(rank_of(s)); }
constexpr Bitboard file_bb(File f) { return FileABB << f; }
constexpr Bitboard file_bb(Square s) { return file_bb(file_of(s)); }

constexpr bool more_than_one(Bitboard b) { return b & (b - 1); }

inline int    popcount(Bitboard b) { return std::popcount(b); }
inline Square lsb(Bitboard b) {
    assert(b);
    return Square(std::countr_zero(b));
}
inline Square msb(Bitboard b) {
    assert(b);
    return Square(63 ^ std::countl_zero(b));
}
inline Square pop_lsb(Bitboard& b) {
    const Square s = lsb(b);
    b &= b - 1;
    return s;
}

template<Direction D>
constexpr Bitboard shift(Bitboard b) {
    return D == NORTH         ? b << 8
         : D == SOUTH         ? b >> 8
         : D == NORTH + NORTH ? b << 16
         : D == SOUTH + SOUTH ? b >> 16
         : D == EAST          ? (b & ~FileHBB) << 1
         : D == WEST          ? (b & ~FileABB) >> 1
         : D == NORTH_EAST    ? (b & ~FileHBB) << 9
         : D == NORTH_WEST    ? (b & ~FileABB) << 7
         : D == SOUTH_EAST    ? (b & ~FileHBB) >> 7
         : D == SOUTH_WEST    ? (b & ~FileABB) >> 9
                              : 0;
}

template<Color C>
constexpr Bitboard pawn_attacks_bb(Bitboard b) {
    return C == WHITE ? shift<NORTH_WEST>(b) | shift<NORTH_EAST>(b)
                      : shift<SOUTH_WEST>(b) | shift<SOUTH_EAST>(b);
}

struct Magic {
    Bitboard  mask;
    Bitboard* attacks;
#if !defined(USE_PEXT)
    Bitboard magic;
    unsigned shift;
#endif

    unsigned index(Bitboard occupied) const {
#if defined(USE_PEXT)
        return unsigned(_pext_u64(occupied, mask));
#else
        return unsigned(((occupied & mask) * magic) >> shift);
#endif
    }
    Bitboard attacks_bb(Bitboard occupied) const { return attacks[index(occupied)]; }
};

extern Magic    RookMagics[SQUARE_NB];
extern Magic    BishopMagics[SQUARE_NB];
extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
// BetweenBB[a][b]: squares strictly between a and b, plus b itself. For
// squares that are not aligned it is just b, which is what check evasion wants.
extern Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
// LineBB[a][b]: the whole line through a and b (edge to edge), or 0.
extern Bitboard LineBB[SQUARE_NB][SQUARE_NB];
extern uint8_t  SquareDistance[SQUARE_NB][SQUARE_NB];

inline Bitboard pawn_attacks_bb(Color c, Square s) { return PawnAttacks[c][s]; }
inline Bitboard between_bb(Square a, Square b) { return BetweenBB[a][b]; }
inline Bitboard line_bb(Square a, Square b) { return LineBB[a][b]; }
inline bool     aligned(Square a, Square b, Square c) { return LineBB[a][b] & square_bb(c); }
inline int      distance(Square a, Square b) { return SquareDistance[a][b]; }

template<PieceType Pt>
inline Bitboard attacks_bb(Square s) {
    return PseudoAttacks[Pt][s];
}

template<PieceType Pt>
inline Bitboard attacks_bb(Square s, Bitboard occupied) {
    switch (Pt)
    {
    case BISHOP :
        return BishopMagics[s].attacks_bb(occupied);
    case ROOK :
        return RookMagics[s].attacks_bb(occupied);
    case QUEEN :
        return BishopMagics[s].attacks_bb(occupied) | RookMagics[s].attacks_bb(occupied);
    default :
        return PseudoAttacks[Pt][s];
    }
}

inline Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied) {
    switch (pt)
    {
    case BISHOP :
        return attacks_bb<BISHOP>(s, occupied);
    case ROOK :
        return attacks_bb<ROOK>(s, occupied);
    case QUEEN :
        return attacks_bb<QUEEN>(s, occupied);
    default :
        return PseudoAttacks[pt][s];
    }
}

}  // namespace Nest
