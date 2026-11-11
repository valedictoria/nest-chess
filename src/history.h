/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "position.h"
#include "types.h"

namespace Nest {

// A history counter that saturates at +-D: each update pulls the value
// towards the bonus and shrinks it in proportion to its size ("gravity").
template<int D>
class StatEntry {
    int16_t entry;

   public:
    void operator=(int16_t v) { entry = v; }
         operator int() const { return entry; }

    void operator<<(int bonus) {
        const int b = std::clamp(bonus, -D, D);
        entry       = int16_t(entry + b - entry * std::abs(b) / D);
    }
};

template<typename T, size_t N, size_t... Rest>
struct Stats : std::array<Stats<T, Rest...>, N> {
    void fill(int v) {
        for (auto& e : *this)
            e.fill(v);
    }
};

template<typename T, size_t N>
struct Stats<T, N> : std::array<T, N> {
    void fill(int v) {
        for (auto& e : *this)
        {
            // The element may itself be a table (tables of tables).
            if constexpr (requires { e.fill(v); })
                e.fill(v);
            else
                e = int16_t(v);
        }
    }
};

constexpr int PawnHistorySize       = 512;   // power of two
constexpr int CorrectionHistorySize = 16384; // power of two
constexpr int CorrectionLimit       = 1024;

inline int pawn_history_index(const Position& pos) { return int(pos.pawn_key() & (PawnHistorySize - 1)); }
inline int correction_index(Key k) { return int(k & (CorrectionHistorySize - 1)); }

// Quiet moves by side and from/to squares.
using ButterflyHistory = Stats<StatEntry<7183>, COLOR_NB, 4096>;
// Captures by moving piece, target square and captured piece type.
using CaptureHistory = Stats<StatEntry<10692>, PIECE_NB, SQUARE_NB, PIECE_TYPE_NB>;
// A move as (piece, target square).
using PieceToHistory = Stats<StatEntry<29952>, PIECE_NB, SQUARE_NB>;
// Pairs of moves: a (piece, to) table for each earlier (piece, to).
using ContinuationHistory = Stats<PieceToHistory, PIECE_NB, SQUARE_NB>;
// Quiet moves in the context of the pawn structure.
using PawnHistory = Stats<StatEntry<8192>, PawnHistorySize, PIECE_NB, SQUARE_NB>;
// Reply to the previous move, by (piece, to) of that move.
using CounterMoves = std::array<std::array<Move, SQUARE_NB>, PIECE_NB>;

// Correction histories: learned differences between static eval and search
// results, indexed by structure keys.
using PawnCorrectionHistory    = Stats<StatEntry<CorrectionLimit>, CorrectionHistorySize, COLOR_NB>;
using NonPawnCorrectionHistory = Stats<StatEntry<CorrectionLimit>, CorrectionHistorySize, COLOR_NB, COLOR_NB>;
using ContinuationCorrectionHistory = Stats<Stats<StatEntry<CorrectionLimit>, PIECE_NB, SQUARE_NB>, PIECE_NB, SQUARE_NB>;
using PieceToCorrectionHistory      = Stats<StatEntry<CorrectionLimit>, PIECE_NB, SQUARE_NB>;

}  // namespace Nest
