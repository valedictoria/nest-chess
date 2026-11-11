/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.

  This file implements the network format of Stockfish 17.1 (GPL-3.0):
  HalfKAv2_hm inputs, a feature transformer with PSQT outputs, and eight
  layer stacks selected by piece count. The format is reproduced from
  Stockfish's src/nnue sources at tag sf_17.1; the code here is our own.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "../bitboard.h"
#include "../types.h"

namespace Nest {
class Position;
}

namespace Nest::NNUE {

constexpr int L1Big       = 3072;
constexpr int L1Small     = 128;
constexpr int L2          = 15;
constexpr int L3          = 32;
constexpr int PSQTBuckets = 8;
constexpr int LayerStacks = 8;
constexpr int InputDims   = 22528;  // 32 king buckets x 11 piece kinds x 64 squares

constexpr int OutputScale     = 16;
constexpr int WeightScaleBits = 6;

constexpr size_t CacheLine = 64;

// --- HalfKAv2_hm feature index ---------------------------------------------

// The board is mirrored horizontally so that the king is on files e-h, and
// seen from the perspective's own side.
inline int feature_index(Color perspective, Square s, Piece pc, Square ksq) {
    const int orient = (file_of(ksq) < FILE_E ? 7 : 0) ^ (perspective == BLACK ? 56 : 0);
    const int kf     = file_of(ksq) < FILE_E ? file_of(ksq) : 7 - file_of(ksq);
    const int kr     = perspective == WHITE ? 7 - rank_of(ksq) : rank_of(ksq);
    const int bucket = kr * 4 + kf;
    const int kind   = type_of(pc) == KING ? 10 : 2 * (type_of(pc) - 1) + (color_of(pc) != perspective);
    return (int(s) ^ orient) + kind * 64 + bucket * 704;
}

// --- Accumulator -------------------------------------------------------------

template<int L1>
struct alignas(CacheLine) Accumulator {
    int16_t v[COLOR_NB][L1];
    int32_t psqt[COLOR_NB][PSQTBuckets];
    bool    computed[COLOR_NB];
};

// --- Network -------------------------------------------------------------------

template<int L1>
struct LayerStack {
    alignas(CacheLine) int32_t b0[L2 + 1];
    alignas(CacheLine) int8_t  w0[L1 * (L2 + 1)];  // [input / 4][output][input % 4]
    alignas(CacheLine) int32_t b1[L3];
    alignas(CacheLine) int8_t  w1[32 * L3];        // same chunked layout, 32 padded inputs
    alignas(CacheLine) int32_t b2;
    alignas(CacheLine) int8_t  w2[L3];
};

template<int L1>
class Network {
   public:
    Network();
    ~Network();

    // Parses a .nnue image. On failure returns false and explains in `error`;
    // the previously loaded parameters are then gone and `loaded()` is false.
    bool load(const uint8_t* data, size_t size, std::string& error);
    bool loaded() const { return isLoaded; }

    // Raw network outputs for the side to move, already divided by the
    // output scale: the PSQT part and the positional part.
    void evaluate(const Accumulator<L1>& acc, Color stm, int bucket, int& psqt, int& positional) const;

    // Feature transformer parameters (accumulator weights are stored doubled,
    // see transform()).
    const int16_t* ft_biases() const { return biases; }
    const int16_t* ft_weights(int feature) const { return weights + size_t(feature) * L1; }
    const int32_t* ft_psqt(int feature) const { return psqtWeights + size_t(feature) * PSQTBuckets; }

    std::string description;

   private:
    void transform(const Accumulator<L1>& acc, Color stm, uint8_t* out) const;

    int16_t*       biases      = nullptr;  // [L1]
    int16_t*       weights     = nullptr;  // [InputDims][L1]
    int32_t*       psqtWeights = nullptr;  // [InputDims][PSQTBuckets]
    LayerStack<L1>* stacks     = nullptr;  // [LayerStacks]
    bool           isLoaded    = false;
};

struct Networks {
    Network<L1Big>   big;
    Network<L1Small> small;
};

// The networks in use, shared read-only by all threads.
extern Networks* networks;

// --- Incremental update state ---------------------------------------------

template<int L1>
struct RefreshEntry {
    alignas(CacheLine) int16_t v[L1];
    int32_t  psqt[PSQTBuckets];
    Bitboard byColor[COLOR_NB];
    Bitboard byType[PIECE_TYPE_NB];
};

// Per king square and perspective: the accumulator of the last position
// refreshed with that king square, and which pieces it was built from. A
// refresh then only has to apply the difference ("Finny tables").
template<int L1>
struct RefreshCache {
    RefreshEntry<L1> entries[SQUARE_NB][COLOR_NB];
    void             clear(const Network<L1>& net);
};

struct AccumulatorState {
    DirtyPiece         dirty;
    Accumulator<L1Big>   big;
    Accumulator<L1Small> small;
};

class AccumulatorStack {
   public:
    AccumulatorStack();
    ~AccumulatorStack();
    AccumulatorStack(const AccumulatorStack&)            = delete;
    AccumulatorStack& operator=(const AccumulatorStack&) = delete;

    // Start of a search: forget everything.
    void reset();

    void push(const DirtyPiece& dp) {
        AccumulatorState& s = states[size++];
        s.dirty             = dp;
        s.big.computed[WHITE] = s.big.computed[BLACK] = false;
        s.small.computed[WHITE] = s.small.computed[BLACK] = false;
    }
    void pop() { --size; }

    // Brings the accumulator of the current position up to date (lazily,
    // from the nearest computed ancestor or from the refresh cache).
    const Accumulator<L1Big>&   big(const Position& pos);
    const Accumulator<L1Small>& small(const Position& pos);

   private:
    template<int L1>
    void update(const Position& pos, const Network<L1>& net, RefreshCache<L1>& cache,
                Accumulator<L1> AccumulatorState::* member, Color perspective);

    AccumulatorState*      states;
    int                    size = 1;
    RefreshCache<L1Big>*   bigCache;
    RefreshCache<L1Small>* smallCache;
};

// From-scratch accumulator, for verification only.
template<int L1>
void refresh_from_scratch(const Position& pos, const Network<L1>& net, Accumulator<L1>& acc);

}  // namespace Nest::NNUE
