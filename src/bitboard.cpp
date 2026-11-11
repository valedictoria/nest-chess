/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "bitboard.h"

#include <algorithm>
#include <cstdlib>

namespace Nest {

Magic    RookMagics[SQUARE_NB];
Magic    BishopMagics[SQUARE_NB];
Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard LineBB[SQUARE_NB][SQUARE_NB];
uint8_t  SquareDistance[SQUARE_NB][SQUARE_NB];

namespace {

#include "magics.inc"

Bitboard RookTable[0x19000];
Bitboard BishopTable[0x1480];

// A step from s by (df, dr), or 0 when it leaves the board.
Bitboard step(Square s, int df, int dr) {
    const int f = file_of(s) + df, r = rank_of(s) + dr;
    return f >= 0 && f < 8 && r >= 0 && r < 8 ? square_bb(make_square(File(f), Rank(r))) : 0;
}

Bitboard sliding_attack(PieceType pt, Square sq, Bitboard occupied) {
    static const int Dirs[2][4][2] = {{{1, 1}, {-1, 1}, {1, -1}, {-1, -1}},
                                      {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    Bitboard attacks = 0;
    for (const auto& d : Dirs[pt == ROOK])
        for (int i = 1; i < 8; ++i)
        {
            const Bitboard b = step(sq, d[0] * i, d[1] * i);
            if (!b)
                break;
            attacks |= b;
            if (b & occupied)
                break;
        }
    return attacks;
}

void init_magics(PieceType pt, Bitboard table[], Magic magics[], const Bitboard numbers[]) {
    size_t offset = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        // Board edges are not part of the relevant occupancy.
        const Bitboard edges =
          ((Rank1BB | Rank8BB) & ~rank_bb(s)) | ((FileABB | FileHBB) & ~file_bb(s));

        Magic& m  = magics[s];
        m.mask    = sliding_attack(pt, s, 0) & ~edges;
        m.attacks = table + offset;
#if !defined(USE_PEXT)
        m.magic = numbers[s];
        m.shift = 64 - popcount(m.mask);
#else
        (void) numbers;
#endif
        // Carry-rippler walk over all subsets of the mask.
        Bitboard b = 0;
        do
        {
            m.attacks[m.index(b)] = sliding_attack(pt, s, b);
            b                     = (b - m.mask) & m.mask;
            ++offset;
        } while (b);
    }
}

}  // namespace

void Bitboards::init() {
    for (Square a = SQ_A1; a <= SQ_H8; ++a)
        for (Square b = SQ_A1; b <= SQ_H8; ++b)
            SquareDistance[a][b] = uint8_t(std::max(std::abs(file_of(a) - file_of(b)),
                                                    std::abs(rank_of(a) - rank_of(b))));

    init_magics(ROOK, RookTable, RookMagics, RookMagicNumbers);
    init_magics(BISHOP, BishopTable, BishopMagics, BishopMagicNumbers);

    static const int KnightSteps[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2},
                                          {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        PawnAttacks[WHITE][s] = step(s, -1, 1) | step(s, 1, 1);
        PawnAttacks[BLACK][s] = step(s, -1, -1) | step(s, 1, -1);

        for (const auto& d : KnightSteps)
            PseudoAttacks[KNIGHT][s] |= step(s, d[0], d[1]);
        for (int df = -1; df <= 1; ++df)
            for (int dr = -1; dr <= 1; ++dr)
                if (df || dr)
                    PseudoAttacks[KING][s] |= step(s, df, dr);

        PseudoAttacks[BISHOP][s] = attacks_bb<BISHOP>(s, 0);
        PseudoAttacks[ROOK][s]   = attacks_bb<ROOK>(s, 0);
        PseudoAttacks[QUEEN][s]  = PseudoAttacks[BISHOP][s] | PseudoAttacks[ROOK][s];
    }

    for (Square a = SQ_A1; a <= SQ_H8; ++a)
        for (Square b = SQ_A1; b <= SQ_H8; ++b)
        {
            for (PieceType pt : {BISHOP, ROOK})
                if (PseudoAttacks[pt][a] & square_bb(b))
                {
                    LineBB[a][b] = (attacks_bb(pt, a, 0) & attacks_bb(pt, b, 0)) | square_bb(a)
                                 | square_bb(b);
                    BetweenBB[a][b] =
                      attacks_bb(pt, a, square_bb(b)) & attacks_bb(pt, b, square_bb(a));
                }
            BetweenBB[a][b] |= square_bb(b);
        }
}

std::string Bitboards::pretty(Bitboard b) {
    std::string s = "+---+---+---+---+---+---+---+---+\n";
    for (Rank r = RANK_8;; r = Rank(r - 1))
    {
        for (File f = FILE_A; f <= FILE_H; ++f)
            s += b & square_bb(make_square(f, r)) ? "| X " : "|   ";
        s += "| " + std::to_string(1 + r) + "\n+---+---+---+---+---+---+---+---+\n";
        if (r == RANK_1)
            break;
    }
    return s + "  a   b   c   d   e   f   g   h\n";
}

}  // namespace Nest
