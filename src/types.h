/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers

  Nest is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. See the LICENSE file.
*/

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace Nest {

using Bitboard = uint64_t;
using Key      = uint64_t;
using Value    = int;
using Depth    = int;

constexpr int MAX_MOVES = 256;
constexpr int MAX_PLY   = 246;

enum Color : int { WHITE, BLACK, COLOR_NB = 2 };

constexpr Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int {
    NO_PIECE_TYPE,
    PAWN,
    KNIGHT,
    BISHOP,
    ROOK,
    QUEEN,
    KING,
    ALL_PIECES    = 0,
    PIECE_TYPE_NB = 8
};

enum Piece : int {
    NO_PIECE,
    W_PAWN = PAWN,
    W_KNIGHT,
    W_BISHOP,
    W_ROOK,
    W_QUEEN,
    W_KING,
    B_PAWN = PAWN + 8,
    B_KNIGHT,
    B_BISHOP,
    B_ROOK,
    B_QUEEN,
    B_KING,
    PIECE_NB = 16
};

constexpr Piece     make_piece(Color c, PieceType pt) { return Piece((c << 3) + pt); }
constexpr PieceType type_of(Piece pc) { return PieceType(pc & 7); }
constexpr Color     color_of(Piece pc) { return Color(pc >> 3); }
constexpr Piece     operator~(Piece pc) { return Piece(pc ^ 8); }

// clang-format off
enum Square : int {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE,
    SQUARE_NB = 64
};
// clang-format on

enum Direction : int {
    NORTH = 8,
    EAST  = 1,
    SOUTH = -8,
    WEST  = -1,

    NORTH_EAST = NORTH + EAST,
    SOUTH_EAST = SOUTH + EAST,
    SOUTH_WEST = SOUTH + WEST,
    NORTH_WEST = NORTH + WEST
};

enum File : int { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H, FILE_NB };
enum Rank : int { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8, RANK_NB };

constexpr Square  make_square(File f, Rank r) { return Square((r << 3) + f); }
constexpr File    file_of(Square s) { return File(s & 7); }
constexpr Rank    rank_of(Square s) { return Rank(s >> 3); }
constexpr Square  operator+(Square s, Direction d) { return Square(int(s) + int(d)); }
constexpr Square  operator-(Square s, Direction d) { return Square(int(s) - int(d)); }
inline Square&    operator+=(Square& s, Direction d) { return s = s + d; }
inline Square&    operator-=(Square& s, Direction d) { return s = s - d; }
inline Square&    operator++(Square& s) { return s = Square(int(s) + 1); }
inline PieceType& operator++(PieceType& p) { return p = PieceType(int(p) + 1); }
inline File&      operator++(File& f) { return f = File(int(f) + 1); }
inline Rank&      operator++(Rank& r) { return r = Rank(int(r) + 1); }

constexpr bool      is_ok(Square s) { return s >= SQ_A1 && s <= SQ_H8; }
constexpr Square    flip_rank(Square s) { return Square(s ^ 56); }
constexpr Square    flip_file(Square s) { return Square(s ^ 7); }
constexpr Rank      relative_rank(Color c, Rank r) { return Rank(r ^ (c * 7)); }
constexpr Rank      relative_rank(Color c, Square s) { return relative_rank(c, rank_of(s)); }
constexpr Square    relative_square(Color c, Square s) { return Square(s ^ (c * 56)); }
constexpr Direction pawn_push(Color c) { return c == WHITE ? NORTH : SOUTH; }

enum CastlingRights : int {
    NO_CASTLING,
    WHITE_OO,
    WHITE_OOO = WHITE_OO << 1,
    BLACK_OO  = WHITE_OO << 2,
    BLACK_OOO = WHITE_OO << 3,

    KING_SIDE      = WHITE_OO | BLACK_OO,
    QUEEN_SIDE     = WHITE_OOO | BLACK_OOO,
    WHITE_CASTLING = WHITE_OO | WHITE_OOO,
    BLACK_CASTLING = BLACK_OO | BLACK_OOO,
    ANY_CASTLING   = WHITE_CASTLING | BLACK_CASTLING,

    CASTLING_RIGHT_NB = 16
};

enum Bound : int {
    BOUND_NONE,
    BOUND_UPPER,
    BOUND_LOWER,
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER
};

constexpr Value VALUE_ZERO     = 0;
constexpr Value VALUE_DRAW     = 0;
constexpr Value VALUE_MATE     = 32000;
constexpr Value VALUE_INFINITE = 32001;
constexpr Value VALUE_NONE     = 32002;

constexpr Value VALUE_MATE_IN_MAX_PLY  = VALUE_MATE - MAX_PLY;
constexpr Value VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY;
// Scores at or beyond this are reserved for tablebase results.
constexpr Value VALUE_TB_WIN_IN_MAX_PLY  = VALUE_MATE_IN_MAX_PLY - MAX_PLY;
constexpr Value VALUE_TB_LOSS_IN_MAX_PLY = -VALUE_TB_WIN_IN_MAX_PLY;
// Largest value a static evaluation may take.
constexpr Value VALUE_MAX_EVAL = VALUE_TB_WIN_IN_MAX_PLY - 1;

constexpr Value mate_in(int ply) { return VALUE_MATE - ply; }
constexpr Value mated_in(int ply) { return -VALUE_MATE + ply; }
constexpr bool  is_decisive(Value v) { return v >= VALUE_TB_WIN_IN_MAX_PLY || v <= VALUE_TB_LOSS_IN_MAX_PLY; }

// Piece values used by SEE, move ordering and the fallback evaluation.
constexpr Value PawnValue   = 208;
constexpr Value KnightValue = 781;
constexpr Value BishopValue = 825;
constexpr Value RookValue   = 1276;
constexpr Value QueenValue  = 2538;

constexpr Value PieceValue[PIECE_NB] = {
  0, PawnValue, KnightValue, BishopValue, RookValue, QueenValue, 0, 0,
  0, PawnValue, KnightValue, BishopValue, RookValue, QueenValue, 0, 0};

constexpr Depth DEPTH_QS     = 0;
constexpr Depth DEPTH_UNSEARCHED = -2;  // TT entry carries only a static eval
constexpr Depth DEPTH_ENTRY_OFFSET = -3;

enum MoveType : int {
    NORMAL,
    PROMOTION  = 1 << 14,
    EN_PASSANT = 2 << 14,
    CASTLING   = 3 << 14
};

// A move fits in 16 bits:
//   bits  0- 5  destination square
//   bits  6-11  origin square
//   bits 12-13  promotion piece type - KNIGHT
//   bits 14-15  move type (normal, promotion, en passant, castling)
// Castling is encoded as "king takes own rook", which also covers Chess960.
class Move {
   public:
    Move() = default;
    constexpr explicit Move(uint16_t d) :
        data(d) {}
    constexpr Move(Square from, Square to) :
        data(uint16_t((from << 6) + to)) {}

    template<MoveType T>
    static constexpr Move make(Square from, Square to, PieceType pt = KNIGHT) {
        return Move(uint16_t(T + ((pt - KNIGHT) << 12) + (from << 6) + to));
    }

    constexpr Square    from_sq() const { return Square((data >> 6) & 0x3F); }
    constexpr Square    to_sq() const { return Square(data & 0x3F); }
    constexpr int       from_to() const { return data & 0xFFF; }
    constexpr MoveType  type_of() const { return MoveType(data & (3 << 14)); }
    constexpr PieceType promotion_type() const { return PieceType(((data >> 12) & 3) + KNIGHT); }
    constexpr uint16_t  raw() const { return data; }

    static constexpr Move none() { return Move(0); }
    static constexpr Move null() { return Move(65); }

    constexpr bool     is_ok() const { return none().data != data && null().data != data; }
    constexpr bool     operator==(const Move& m) const { return data == m.data; }
    constexpr bool     operator!=(const Move& m) const { return data != m.data; }
    constexpr explicit operator bool() const { return data != 0; }

   private:
    uint16_t data;
};

// What a move changed on the board, as needed by incremental evaluation.
// A square of SQ_NONE means the piece appeared (from) or vanished (to).
struct DirtyPiece {
    int    count;
    Piece  pc[3];
    Square from[3];
    Square to[3];
};

}  // namespace Nest
