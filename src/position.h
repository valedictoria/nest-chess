/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <string>

#include "bitboard.h"
#include "types.h"

namespace Nest {

// Everything needed to take a move back, plus data derived from the position
// that is cheaper to keep than to recompute. The fields above `key` are
// copied from the previous state in do_move(); the rest is rebuilt.
struct StateInfo {
    Key    pawnKey;
    Key    materialKey;
    Key    nonPawnKey[COLOR_NB];
    Value  nonPawnMaterial[COLOR_NB];
    int    castlingRights;
    int    rule50;
    int    pliesFromNull;
    Square epSquare;

    Key        key;
    Bitboard   checkers;
    StateInfo* previous;
    Bitboard   blockersForKing[COLOR_NB];
    Bitboard   pinners[COLOR_NB];
    Bitboard   checkSquares[PIECE_TYPE_NB];
    Piece      capturedPiece;
    int        repetition;
};

class Position {
   public:
    static void init();

    Position()                           = default;
    Position(const Position&)            = delete;
    Position& operator=(const Position&) = delete;

    // Returns false (leaving the position unspecified) on a malformed FEN.
    bool        set(const std::string& fen, bool isChess960, StateInfo* si);
    std::string fen() const;
    std::string pretty() const;

    Bitboard pieces() const { return byType[ALL_PIECES]; }
    Bitboard pieces(PieceType pt) const { return byType[pt]; }
    Bitboard pieces(PieceType a, PieceType b) const { return byType[a] | byType[b]; }
    Bitboard pieces(Color c) const { return byColor[c]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColor[c] & byType[pt]; }
    Bitboard pieces(Color c, PieceType a, PieceType b) const {
        return byColor[c] & (byType[a] | byType[b]);
    }
    Piece  piece_on(Square s) const { return board[s]; }
    bool   empty(Square s) const { return board[s] == NO_PIECE; }
    int    count(Piece pc) const { return pieceCount[pc]; }
    int    count(Color c, PieceType pt) const { return pieceCount[make_piece(c, pt)]; }
    Square king_square(Color c) const { return lsb(pieces(c, KING)); }
    Square ep_square() const { return st->epSquare; }
    Color  side_to_move() const { return sideToMove; }
    int    game_ply() const { return gamePly; }
    bool   is_chess960() const { return chess960; }

    int    castling_rights() const { return st->castlingRights; }
    bool   can_castle(int cr) const { return st->castlingRights & cr; }
    bool   castling_impeded(CastlingRights cr) const { return pieces() & castlingPath[cr]; }
    Square castling_rook_square(CastlingRights cr) const { return castlingRookSquare[cr]; }

    Bitboard checkers() const { return st->checkers; }
    Bitboard blockers_for_king(Color c) const { return st->blockersForKing[c]; }
    Bitboard pinners(Color c) const { return st->pinners[c]; }
    Bitboard check_squares(PieceType pt) const { return st->checkSquares[pt]; }

    Bitboard attackers_to(Square s) const { return attackers_to(s, pieces()); }
    Bitboard attackers_to(Square s, Bitboard occupied) const;
    // Is square s attacked by colour c, given the occupancy?
    bool attacked_by(Color c, Square s, Bitboard occupied) const;

    // pseudo_legal() accepts any 16-bit value, e.g. a move read from a
    // possibly corrupted transposition-table entry. A move for which both
    // pseudo_legal() and legal() hold is legal. Moves from the generator are
    // already legal and need neither.
    bool pseudo_legal(Move m) const;
    bool legal(Move m) const;
    bool gives_check(Move m) const;
    bool capture(Move m) const {
        return (!empty(m.to_sq()) && m.type_of() != CASTLING) || m.type_of() == EN_PASSANT;
    }
    // Captures and queen promotions: the moves of the "captures" stage.
    bool capture_stage(Move m) const {
        return capture(m) || (m.type_of() == PROMOTION && m.promotion_type() == QUEEN);
    }
    Piece moved_piece(Move m) const { return piece_on(m.from_sq()); }
    Piece captured_piece() const { return st->capturedPiece; }

    void do_move(Move m, StateInfo& newSt, bool givesCheck, DirtyPiece& dp);
    void do_move(Move m, StateInfo& newSt) {
        DirtyPiece dp;
        do_move(m, newSt, gives_check(m), dp);
    }
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    // Static exchange evaluation: is the exchange started by m worth at
    // least `threshold`?
    bool see_ge(Move m, int threshold = 0) const;

    Key   key() const { return st->key; }
    Key   pawn_key() const { return st->pawnKey; }
    Key   material_key() const { return st->materialKey; }
    Key   non_pawn_key(Color c) const { return st->nonPawnKey[c]; }
    Value non_pawn_material(Color c) const { return st->nonPawnMaterial[c]; }
    Value non_pawn_material() const { return st->nonPawnMaterial[WHITE] + st->nonPawnMaterial[BLACK]; }
    int   rule50_count() const { return st->rule50; }
    int   plies_from_null() const { return st->pliesFromNull; }

    // Draw by the 50-move rule or by repetition. A position repeated once
    // inside the search tree (after the root) already counts.
    bool is_draw(int ply) const;
    bool has_repeated() const;
    bool insufficient_material() const;

    StateInfo* state() const { return st; }

    // Debug: recomputes every incrementally maintained field and compares.
    bool consistent() const;

   private:
    void set_castling_right(Color c, Square rfrom);
    void set_state();
    void set_check_info();
    Bitboard slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const;

    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);
    template<bool Do>
    void do_castling(Color us, Square from, Square& to, Square& rfrom, Square& rto);

    Piece      board[SQUARE_NB];
    Bitboard   byType[PIECE_TYPE_NB];
    Bitboard   byColor[COLOR_NB];
    int        pieceCount[PIECE_NB];
    int        castlingRightsMask[SQUARE_NB];
    Square     castlingRookSquare[CASTLING_RIGHT_NB];
    Bitboard   castlingPath[CASTLING_RIGHT_NB];
    StateInfo* st;
    int        gamePly;
    Color      sideToMove;
    bool       chess960;
};

inline void Position::put_piece(Piece pc, Square s) {
    board[s] = pc;
    byType[ALL_PIECES] |= byType[type_of(pc)] |= square_bb(s);
    byColor[color_of(pc)] |= square_bb(s);
    ++pieceCount[pc];
}

inline void Position::remove_piece(Square s) {
    const Piece pc = board[s];
    byType[ALL_PIECES] ^= square_bb(s);
    byType[type_of(pc)] ^= square_bb(s);
    byColor[color_of(pc)] ^= square_bb(s);
    board[s] = NO_PIECE;
    --pieceCount[pc];
}

inline void Position::move_piece(Square from, Square to) {
    const Piece    pc     = board[from];
    const Bitboard fromTo = square_bb(from) | square_bb(to);
    byType[ALL_PIECES] ^= fromTo;
    byType[type_of(pc)] ^= fromTo;
    byColor[color_of(pc)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to]   = pc;
}

inline Bitboard Position::attackers_to(Square s, Bitboard occupied) const {
    return (pawn_attacks_bb(BLACK, s) & pieces(WHITE, PAWN))
         | (pawn_attacks_bb(WHITE, s) & pieces(BLACK, PAWN))
         | (attacks_bb<KNIGHT>(s) & pieces(KNIGHT))
         | (attacks_bb<ROOK>(s, occupied) & pieces(ROOK, QUEEN))
         | (attacks_bb<BISHOP>(s, occupied) & pieces(BISHOP, QUEEN))
         | (attacks_bb<KING>(s) & pieces(KING));
}

inline bool Position::attacked_by(Color c, Square s, Bitboard occupied) const {
    return (pawn_attacks_bb(~c, s) & pieces(c, PAWN)) || (attacks_bb<KNIGHT>(s) & pieces(c, KNIGHT))
        || (attacks_bb<KING>(s) & pieces(c, KING))
        || (attacks_bb<BISHOP>(s, occupied) & pieces(c, BISHOP, QUEEN))
        || (attacks_bb<ROOK>(s, occupied) & pieces(c, ROOK, QUEEN));
}

namespace Zobrist {
extern Key psq[PIECE_NB][SQUARE_NB];
extern Key enpassant[FILE_NB];
extern Key castling[CASTLING_RIGHT_NB];
extern Key side;
}

}  // namespace Nest
