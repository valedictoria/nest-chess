/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.

  The state-stack design, the castling encoding and the exchange loop in
  see_ge() follow the approach used by Stockfish (GPL-3.0).
*/

#include "position.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "misc.h"
#include "movegen.h"

namespace Nest {

namespace Zobrist {
Key psq[PIECE_NB][SQUARE_NB];
Key enpassant[FILE_NB];
Key castling[CASTLING_RIGHT_NB];
Key side;
}

namespace {
constexpr char PieceToChar[] = " PNBRQK  pnbrqk";

constexpr Piece Pieces[] = {W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                            B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING};
}

void Position::init() {
    PRNG rng(0x4E455354C0FFEEULL);
    for (Piece pc : Pieces)
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
            Zobrist::psq[pc][s] = rng.next();
    for (File f = FILE_A; f <= FILE_H; ++f)
        Zobrist::enpassant[f] = rng.next();
    for (int cr = 0; cr < CASTLING_RIGHT_NB; ++cr)
        Zobrist::castling[cr] = rng.next();
    Zobrist::side = rng.next();
}

bool Position::set(const std::string& fenStr, bool isChess960, StateInfo* si) {
    std::memset(static_cast<void*>(this), 0, sizeof(Position));
    std::memset(static_cast<void*>(si), 0, sizeof(StateInfo));
    st       = si;
    chess960 = isChess960;

    std::istringstream ss(fenStr);
    std::string        placement, stm, castling, ep;
    if (!(ss >> placement >> stm))
        return false;
    if (!(ss >> castling))
        castling = "-";
    if (!(ss >> ep))
        ep = "-";

    int f = 0, r = 7;
    for (char c : placement)
    {
        if (c == '/')
        {
            if (f != 8 || --r < 0)
                return false;
            f = 0;
        }
        else if (c >= '1' && c <= '8')
            f += c - '0';
        else
        {
            const char* p = std::strchr(PieceToChar, c);
            if (!p || c == ' ' || f > 7)
                return false;
            put_piece(Piece(p - PieceToChar), make_square(File(f), Rank(r)));
            ++f;
        }
        if (f > 8)
            return false;
    }
    if (r != 0 || f != 8 || count(W_KING) != 1 || count(B_KING) != 1)
        return false;
    if ((pieces(PAWN) & (Rank1BB | Rank8BB)) || count(W_PAWN) > 8 || count(B_PAWN) > 8
        || popcount(pieces(WHITE)) > 16 || popcount(pieces(BLACK)) > 16)
        return false;

    if (stm != "w" && stm != "b")
        return false;
    sideToMove = stm == "w" ? WHITE : BLACK;

    // Castling availability: KQkq, Shredder-FEN file letters, or X-FEN.
    for (char c : castling)
    {
        if (c == '-')
            continue;
        const Color  col  = (c >= 'a' && c <= 'z') ? BLACK : WHITE;
        const Piece  rook = make_piece(col, ROOK);
        const Square ksq  = king_square(col);
        const char   u    = char(col == BLACK ? c - 'a' + 'A' : c);
        Square       rsq  = SQ_NONE;

        if (relative_rank(col, ksq) != RANK_1)
            continue;
        if (u == 'K')
        {
            for (Square s = relative_square(col, SQ_H1); s > ksq; s = s - EAST)
                if (piece_on(s) == rook)
                {
                    rsq = s;
                    break;
                }
        }
        else if (u == 'Q')
        {
            for (Square s = relative_square(col, SQ_A1); s < ksq; s = s + EAST)
                if (piece_on(s) == rook)
                {
                    rsq = s;
                    break;
                }
        }
        else if (u >= 'A' && u <= 'H')
        {
            const Square s = make_square(File(u - 'A'), relative_rank(col, RANK_1));
            if (piece_on(s) == rook)
                rsq = s;
        }
        else
            return false;

        if (rsq != SQ_NONE)
            set_castling_right(col, rsq);
    }

    st->epSquare = SQ_NONE;
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' && (ep[1] == '3' || ep[1] == '6'))
    {
        const Square s    = make_square(File(ep[0] - 'a'), Rank(ep[1] - '1'));
        const Color  them = ~sideToMove;
        // Keep the square only if a capture is at least pseudo-legal, so that
        // equal positions get equal keys.
        if (relative_rank(sideToMove, s) == RANK_6
            && (pawn_attacks_bb(them, s) & pieces(sideToMove, PAWN))
            && (pieces(them, PAWN) & square_bb(s + pawn_push(them))) && empty(s)
            && empty(s + pawn_push(sideToMove)))
            st->epSquare = s;
    }

    int fullmove = 1;
    st->rule50   = 0;
    if (ss >> st->rule50)
        ss >> fullmove;
    st->rule50 = std::clamp(st->rule50, 0, 150);
    gamePly    = std::max(2 * (std::clamp(fullmove, 1, 5000) - 1), 0) + (sideToMove == BLACK);

    set_state();

    // The side that just moved must not be in check.
    if (attackers_to(king_square(~sideToMove)) & pieces(sideToMove))
        return false;
    return true;
}

void Position::set_castling_right(Color c, Square rfrom) {
    const Square kfrom = king_square(c);
    const auto   cr    = CastlingRights(WHITE_OO << ((kfrom > rfrom) + 2 * c));

    st->castlingRights |= cr;
    castlingRightsMask[kfrom] |= cr;
    castlingRightsMask[rfrom] |= cr;
    castlingRookSquare[cr] = rfrom;

    const Square kto = relative_square(c, kfrom < rfrom ? SQ_G1 : SQ_C1);
    const Square rto = relative_square(c, kfrom < rfrom ? SQ_F1 : SQ_D1);

    castlingPath[cr] =
      (between_bb(rfrom, rto) | between_bb(kfrom, kto)) & ~(square_bb(kfrom) | square_bb(rfrom));
}

// Computes all hash keys and derived state from scratch.
void Position::set_state() {
    st->key = st->pawnKey = st->materialKey = 0;
    st->nonPawnKey[WHITE] = st->nonPawnKey[BLACK] = 0;
    st->nonPawnMaterial[WHITE] = st->nonPawnMaterial[BLACK] = 0;
    st->checkers = attackers_to(king_square(sideToMove)) & pieces(~sideToMove);

    set_check_info();

    for (Bitboard b = pieces(); b;)
    {
        const Square s  = pop_lsb(b);
        const Piece  pc = piece_on(s);
        st->key ^= Zobrist::psq[pc][s];
        if (type_of(pc) == PAWN)
            st->pawnKey ^= Zobrist::psq[pc][s];
        else
        {
            st->nonPawnKey[color_of(pc)] ^= Zobrist::psq[pc][s];
            st->nonPawnMaterial[color_of(pc)] += PieceValue[pc];
        }
    }
    if (st->epSquare != SQ_NONE)
        st->key ^= Zobrist::enpassant[file_of(st->epSquare)];
    if (sideToMove == BLACK)
        st->key ^= Zobrist::side;
    st->key ^= Zobrist::castling[st->castlingRights];

    for (Piece pc : Pieces)
        for (int cnt = 0; cnt < pieceCount[pc]; ++cnt)
            st->materialKey ^= Zobrist::psq[pc][cnt];
}

Bitboard Position::slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const {
    Bitboard blockers = 0;
    pinners           = 0;

    Bitboard snipers = ((attacks_bb<ROOK>(s) & pieces(QUEEN, ROOK))
                        | (attacks_bb<BISHOP>(s) & pieces(QUEEN, BISHOP)))
                     & sliders;
    const Bitboard occupancy = pieces() ^ snipers;

    while (snipers)
    {
        const Square   sniperSq = pop_lsb(snipers);
        const Bitboard b        = between_bb(s, sniperSq) & occupancy;
        if (b && !more_than_one(b))
        {
            blockers |= b;
            if (b & pieces(color_of(piece_on(s))))
                pinners |= square_bb(sniperSq);
        }
    }
    return blockers;
}

void Position::set_check_info() {
    st->blockersForKing[WHITE] =
      slider_blockers(pieces(BLACK), king_square(WHITE), st->pinners[BLACK]);
    st->blockersForKing[BLACK] =
      slider_blockers(pieces(WHITE), king_square(BLACK), st->pinners[WHITE]);

    const Square ksq = king_square(~sideToMove);

    st->checkSquares[PAWN]   = pawn_attacks_bb(~sideToMove, ksq);
    st->checkSquares[KNIGHT] = attacks_bb<KNIGHT>(ksq);
    st->checkSquares[BISHOP] = attacks_bb<BISHOP>(ksq, pieces());
    st->checkSquares[ROOK]   = attacks_bb<ROOK>(ksq, pieces());
    st->checkSquares[QUEEN]  = st->checkSquares[BISHOP] | st->checkSquares[ROOK];
    st->checkSquares[KING]   = 0;
}

std::string Position::fen() const {
    std::ostringstream ss;

    for (Rank r = RANK_8;; r = Rank(r - 1))
    {
        for (File f = FILE_A; f <= FILE_H;)
        {
            int emptyCnt = 0;
            for (; f <= FILE_H && empty(make_square(f, r)); ++f)
                ++emptyCnt;
            if (emptyCnt)
                ss << emptyCnt;
            if (f <= FILE_H)
            {
                ss << PieceToChar[piece_on(make_square(f, r))];
                ++f;
            }
        }
        if (r == RANK_1)
            break;
        ss << '/';
    }

    ss << (sideToMove == WHITE ? " w " : " b ");

    auto rookChar = [&](CastlingRights cr, char std_, Color c) {
        if (!chess960)
            return std_;
        const char f = char('a' + file_of(castling_rook_square(cr)));
        return c == WHITE ? char(f - 'a' + 'A') : f;
    };
    if (can_castle(WHITE_OO))
        ss << rookChar(WHITE_OO, 'K', WHITE);
    if (can_castle(WHITE_OOO))
        ss << rookChar(WHITE_OOO, 'Q', WHITE);
    if (can_castle(BLACK_OO))
        ss << rookChar(BLACK_OO, 'k', BLACK);
    if (can_castle(BLACK_OOO))
        ss << rookChar(BLACK_OOO, 'q', BLACK);
    if (!can_castle(ANY_CASTLING))
        ss << '-';

    ss << ' ';
    if (ep_square() == SQ_NONE)
        ss << '-';
    else
        ss << char('a' + file_of(ep_square())) << char('1' + rank_of(ep_square()));
    ss << ' ' << st->rule50 << ' ' << 1 + (gamePly - (sideToMove == BLACK)) / 2;
    return ss.str();
}

std::string Position::pretty() const {
    std::ostringstream ss;
    ss << "\n +---+---+---+---+---+---+---+---+\n";
    for (Rank r = RANK_8;; r = Rank(r - 1))
    {
        for (File f = FILE_A; f <= FILE_H; ++f)
            ss << " | " << PieceToChar[piece_on(make_square(f, r))];
        ss << " | " << (1 + r) << "\n +---+---+---+---+---+---+---+---+\n";
        if (r == RANK_1)
            break;
    }
    ss << "   a   b   c   d   e   f   g   h\n\nFen: " << fen() << "\nKey: " << std::hex
       << std::uppercase << key() << std::dec << "\n";
    return ss.str();
}

bool Position::legal(Move m) const {
    if (m.type_of() != NORMAL)
        return true;  // only reachable through the legal generator

    const Color  us   = sideToMove;
    const Square from = m.from_sq(), to = m.to_sq();

    if (type_of(piece_on(from)) == KING)
        return !attacked_by(~us, to, pieces() ^ square_bb(from));

    return !(blockers_for_king(us) & square_bb(from)) || aligned(from, to, king_square(us));
}

bool Position::pseudo_legal(Move m) const {
    const Color  us   = sideToMove;
    const Square from = m.from_sq(), to = m.to_sq();
    const Piece  pc   = piece_on(from);

    // Promotions, en passant and castling are rare: ask the generator.
    if (m.type_of() != NORMAL)
        return checkers() ? MoveList<EVASIONS>(*this).contains(m)
                          : MoveList<CAPTURES>(*this).contains(m)
                              || MoveList<QUIETS>(*this).contains(m);

    if (m.promotion_type() != KNIGHT)  // unused bits must be clear
        return false;
    if (pc == NO_PIECE || color_of(pc) != us)
        return false;
    if (pieces(us) & square_bb(to))
        return false;

    if (type_of(pc) == PAWN)
    {
        if ((Rank8BB | Rank1BB) & square_bb(to))
            return false;
        const Direction up = pawn_push(us);
        if (!(pawn_attacks_bb(us, from) & pieces(~us) & square_bb(to))
            && !((from + up == to) && empty(to))
            && !((from + up + up == to) && relative_rank(us, from) == RANK_2 && empty(to)
                 && empty(to - up)))
            return false;
    }
    else if (!(attacks_bb(type_of(pc), from, pieces()) & square_bb(to)))
        return false;

    if (checkers() && type_of(pc) != KING)
    {
        if (more_than_one(checkers()))
            return false;
        // Must capture the checker or interpose.
        if (!(between_bb(king_square(us), lsb(checkers())) & square_bb(to)))
            return false;
    }
    return true;
}

bool Position::gives_check(Move m) const {
    const Color  us   = sideToMove;
    const Square from = m.from_sq(), to = m.to_sq();
    const Square ksq  = king_square(~us);

    if (m.type_of() == CASTLING)
    {
        const Square   kto = relative_square(us, to > from ? SQ_G1 : SQ_C1);
        const Square   rto = relative_square(us, to > from ? SQ_F1 : SQ_D1);
        const Bitboard occ =
          (pieces() ^ square_bb(from) ^ square_bb(to)) | square_bb(kto) | square_bb(rto);
        return attacks_bb<ROOK>(rto, occ) & square_bb(ksq);
    }

    // Direct check.
    if (check_squares(type_of(piece_on(from))) & square_bb(to))
        return true;

    // Discovered check.
    if ((blockers_for_king(~us) & square_bb(from)) && !aligned(from, to, ksq))
        return true;

    switch (m.type_of())
    {
    case PROMOTION :
        return attacks_bb(m.promotion_type(), to, pieces() ^ square_bb(from)) & square_bb(ksq);

    case EN_PASSANT : {
        // The captured pawn may uncover a check too.
        const Square   capsq = make_square(file_of(to), rank_of(from));
        const Bitboard b     = (pieces() ^ square_bb(from) ^ square_bb(capsq)) | square_bb(to);
        return (attacks_bb<ROOK>(ksq, b) & pieces(us, QUEEN, ROOK))
             | (attacks_bb<BISHOP>(ksq, b) & pieces(us, QUEEN, BISHOP));
    }
    default :
        return false;
    }
}

template<bool Do>
void Position::do_castling(Color us, Square from, Square& to, Square& rfrom, Square& rto) {
    const bool kingSide = to > from;
    rfrom               = to;  // castling is encoded as king takes rook
    rto                 = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
    to                  = relative_square(us, kingSide ? SQ_G1 : SQ_C1);

    // Remove both first: in Chess960 the squares may overlap.
    remove_piece(Do ? from : to);
    remove_piece(Do ? rfrom : rto);
    put_piece(make_piece(us, KING), Do ? to : from);
    put_piece(make_piece(us, ROOK), Do ? rto : rfrom);
}

void Position::do_move(Move m, StateInfo& newSt, bool givesCheck, DirtyPiece& dp) {
    assert(m.is_ok());
    assert(&newSt != st);

    Key k = st->key ^ Zobrist::side;

    std::memcpy(static_cast<void*>(&newSt), st, offsetof(StateInfo, key));
    newSt.previous = st;
    st             = &newSt;

    ++gamePly;
    ++st->rule50;
    ++st->pliesFromNull;

    const Color  us   = sideToMove;
    const Color  them = ~us;
    const Square from = m.from_sq();
    Square       to   = m.to_sq();
    const Piece  pc   = piece_on(from);
    Piece captured    = m.type_of() == EN_PASSANT ? make_piece(them, PAWN) : piece_on(to);

    assert(color_of(pc) == us);
    assert(type_of(captured) != KING);

    dp.count   = 1;
    dp.pc[0]   = pc;
    dp.from[0] = from;

    if (m.type_of() == CASTLING)
    {
        Square rfrom, rto;
        do_castling<true>(us, from, to, rfrom, rto);

        const Piece rook = make_piece(us, ROOK);
        dp.count         = 2;
        dp.pc[1]         = rook;
        dp.from[1]       = rfrom;
        dp.to[1]         = rto;

        const Key rk = Zobrist::psq[rook][rfrom] ^ Zobrist::psq[rook][rto];
        k ^= rk;
        st->nonPawnKey[us] ^= rk;
        captured = NO_PIECE;
    }
    dp.to[0] = to;

    if (captured)
    {
        Square capsq = to;

        if (type_of(captured) == PAWN)
        {
            if (m.type_of() == EN_PASSANT)
                capsq = capsq - pawn_push(us);
            st->pawnKey ^= Zobrist::psq[captured][capsq];
        }
        else
        {
            st->nonPawnMaterial[them] -= PieceValue[captured];
            st->nonPawnKey[them] ^= Zobrist::psq[captured][capsq];
        }

        dp.count   = 2;
        dp.pc[1]   = captured;
        dp.from[1] = capsq;
        dp.to[1]   = SQ_NONE;

        remove_piece(capsq);
        k ^= Zobrist::psq[captured][capsq];
        st->materialKey ^= Zobrist::psq[captured][pieceCount[captured]];
        st->rule50 = 0;
    }

    k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

    if (st->epSquare != SQ_NONE)
    {
        k ^= Zobrist::enpassant[file_of(st->epSquare)];
        st->epSquare = SQ_NONE;
    }

    if (st->castlingRights && (castlingRightsMask[from] | castlingRightsMask[to]))
    {
        k ^= Zobrist::castling[st->castlingRights];
        st->castlingRights &= ~(castlingRightsMask[from] | castlingRightsMask[to]);
        k ^= Zobrist::castling[st->castlingRights];
    }

    if (m.type_of() != CASTLING)
        move_piece(from, to);

    if (type_of(pc) == PAWN)
    {
        if ((int(to) ^ int(from)) == 16)
        {
            // Double push: set the en-passant square only if it can be used.
            if (pawn_attacks_bb(us, to - pawn_push(us)) & pieces(them, PAWN))
            {
                st->epSquare = to - pawn_push(us);
                k ^= Zobrist::enpassant[file_of(st->epSquare)];
            }
        }
        else if (m.type_of() == PROMOTION)
        {
            const Piece promotion = make_piece(us, m.promotion_type());

            remove_piece(to);
            put_piece(promotion, to);

            dp.to[0]          = SQ_NONE;
            dp.pc[dp.count]   = promotion;
            dp.from[dp.count] = SQ_NONE;
            dp.to[dp.count]   = to;
            dp.count++;

            k ^= Zobrist::psq[pc][to] ^ Zobrist::psq[promotion][to];
            st->pawnKey ^= Zobrist::psq[pc][to];
            st->nonPawnKey[us] ^= Zobrist::psq[promotion][to];
            st->materialKey ^=
              Zobrist::psq[promotion][pieceCount[promotion] - 1] ^ Zobrist::psq[pc][pieceCount[pc]];
            st->nonPawnMaterial[us] += PieceValue[promotion];
        }

        st->pawnKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
        st->rule50 = 0;
    }
    else
        st->nonPawnKey[us] ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

    st->capturedPiece = captured;
    st->key           = k;
    st->checkers      = givesCheck ? attackers_to(king_square(them)) & pieces(us) : 0;

    sideToMove = them;

    set_check_info();

    // Distance to the previous occurrence of this position, negative when
    // that occurrence was itself a repetition.
    st->repetition = 0;
    const int end  = std::min(st->rule50, st->pliesFromNull);
    if (end >= 4)
    {
        StateInfo* stp = st->previous->previous;
        for (int i = 4; i <= end; i += 2)
        {
            stp = stp->previous->previous;
            if (stp->key == st->key)
            {
                st->repetition = stp->repetition ? -i : i;
                break;
            }
        }
    }

    assert(consistent());
}

void Position::undo_move(Move m) {
    sideToMove = ~sideToMove;

    const Color  us   = sideToMove;
    const Square from = m.from_sq();
    Square       to   = m.to_sq();

    if (m.type_of() == PROMOTION)
    {
        remove_piece(to);
        put_piece(make_piece(us, PAWN), to);
    }

    if (m.type_of() == CASTLING)
    {
        Square rfrom, rto;
        do_castling<false>(us, from, to, rfrom, rto);
    }
    else
    {
        move_piece(to, from);

        if (st->capturedPiece)
        {
            Square capsq = to;
            if (m.type_of() == EN_PASSANT)
                capsq = capsq - pawn_push(us);
            put_piece(st->capturedPiece, capsq);
        }
    }

    st = st->previous;
    --gamePly;

    assert(consistent());
}

void Position::do_null_move(StateInfo& newSt) {
    assert(!checkers());

    std::memcpy(static_cast<void*>(&newSt), st, sizeof(StateInfo));
    newSt.previous = st;
    st             = &newSt;

    if (st->epSquare != SQ_NONE)
    {
        st->key ^= Zobrist::enpassant[file_of(st->epSquare)];
        st->epSquare = SQ_NONE;
    }
    st->key ^= Zobrist::side;
    ++st->rule50;
    st->pliesFromNull = 0;

    sideToMove = ~sideToMove;
    set_check_info();
    st->repetition = 0;
}

void Position::undo_null_move() {
    st         = st->previous;
    sideToMove = ~sideToMove;
}

bool Position::see_ge(Move m, int threshold) const {
    // Promotions, en passant and castling are scored as an even trade.
    if (m.type_of() != NORMAL)
        return 0 >= threshold;

    const Square from = m.from_sq(), to = m.to_sq();

    int swap = PieceValue[piece_on(to)] - threshold;
    if (swap < 0)
        return false;

    swap = PieceValue[piece_on(from)] - swap;
    if (swap <= 0)
        return true;

    Bitboard occupied  = pieces() ^ square_bb(from) ^ square_bb(to);
    Color    stm       = sideToMove;
    Bitboard attackers = attackers_to(to, occupied);
    Bitboard stmAttackers, bb;
    int      res = 1;

    while (true)
    {
        stm = ~stm;
        attackers &= occupied;

        if (!(stmAttackers = attackers & pieces(stm)))
            break;

        // Pinned pieces cannot join while their pinners are still in place.
        if (pinners(~stm) & occupied)
        {
            stmAttackers &= ~blockers_for_king(stm);
            if (!stmAttackers)
                break;
        }

        res ^= 1;

        // Capture with the least valuable piece, then add x-ray attackers.
        if ((bb = stmAttackers & pieces(PAWN)))
        {
            if ((swap = PawnValue - swap) < res)
                break;
            occupied ^= bb & -bb;
            attackers |= attacks_bb<BISHOP>(to, occupied) & pieces(BISHOP, QUEEN);
        }
        else if ((bb = stmAttackers & pieces(KNIGHT)))
        {
            if ((swap = KnightValue - swap) < res)
                break;
            occupied ^= bb & -bb;
        }
        else if ((bb = stmAttackers & pieces(BISHOP)))
        {
            if ((swap = BishopValue - swap) < res)
                break;
            occupied ^= bb & -bb;
            attackers |= attacks_bb<BISHOP>(to, occupied) & pieces(BISHOP, QUEEN);
        }
        else if ((bb = stmAttackers & pieces(ROOK)))
        {
            if ((swap = RookValue - swap) < res)
                break;
            occupied ^= bb & -bb;
            attackers |= attacks_bb<ROOK>(to, occupied) & pieces(ROOK, QUEEN);
        }
        else if ((bb = stmAttackers & pieces(QUEEN)))
        {
            swap = QueenValue - swap;
            occupied ^= bb & -bb;
            attackers |= (attacks_bb<BISHOP>(to, occupied) & pieces(BISHOP, QUEEN))
                       | (attacks_bb<ROOK>(to, occupied) & pieces(ROOK, QUEEN));
        }
        else
            // King: legal only if the other side has no attacker left.
            return (attackers & ~pieces(stm)) ? res ^ 1 : res;
    }

    return bool(res);
}

bool Position::is_draw(int ply) const {
    if (st->rule50 > 99 && (!checkers() || MoveList<EVASIONS>(*this).size()))
        return true;
    // A repetition inside the search tree, or a third occurrence overall.
    return st->repetition && st->repetition < ply;
}

bool Position::has_repeated() const {
    StateInfo* stc = st;
    int        end = std::min(st->rule50, st->pliesFromNull);
    while (end-- >= 4)
    {
        if (stc->repetition)
            return true;
        stc = stc->previous;
    }
    return false;
}

bool Position::insufficient_material() const {
    if (pieces(PAWN) | pieces(ROOK, QUEEN))
        return false;
    // K v K, K+minor v K.
    return popcount(pieces()) <= 3;
}

bool Position::consistent() const {
    Key      k = 0, pk = 0, npk[2] = {0, 0}, mk = 0;
    Value    npm[2] = {0, 0};
    Bitboard occ    = 0;
    int      cnt[PIECE_NB] = {};

    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        const Piece pc = board[s];
        if (pc == NO_PIECE)
            continue;
        occ |= square_bb(s);
        ++cnt[pc];
        if (!(byType[type_of(pc)] & byColor[color_of(pc)] & square_bb(s)))
            return false;
        k ^= Zobrist::psq[pc][s];
        if (type_of(pc) == PAWN)
            pk ^= Zobrist::psq[pc][s];
        else
            npk[color_of(pc)] ^= Zobrist::psq[pc][s], npm[color_of(pc)] += PieceValue[pc];
    }
    for (Piece pc : Pieces)
    {
        if (cnt[pc] != pieceCount[pc])
            return false;
        for (int i = 0; i < cnt[pc]; ++i)
            mk ^= Zobrist::psq[pc][i];
    }
    if (st->epSquare != SQ_NONE)
        k ^= Zobrist::enpassant[file_of(st->epSquare)];
    if (sideToMove == BLACK)
        k ^= Zobrist::side;
    k ^= Zobrist::castling[st->castlingRights];

    return occ == byType[ALL_PIECES] && (byColor[WHITE] | byColor[BLACK]) == occ
        && !(byColor[WHITE] & byColor[BLACK]) && k == st->key && pk == st->pawnKey
        && mk == st->materialKey && npk[WHITE] == st->nonPawnKey[WHITE]
        && npk[BLACK] == st->nonPawnKey[BLACK] && npm[WHITE] == st->nonPawnMaterial[WHITE]
        && npm[BLACK] == st->nonPawnMaterial[BLACK]
        && st->checkers == (attackers_to(king_square(sideToMove)) & pieces(~sideToMove))
        && !(attackers_to(king_square(~sideToMove)) & pieces(sideToMove));
}

}  // namespace Nest
