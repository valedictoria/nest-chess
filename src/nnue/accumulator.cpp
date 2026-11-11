/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../misc.h"
#include "../position.h"
#include "nnue.h"

namespace Nest::NNUE {

namespace {

// The loops below are written so that compilers vectorise them for every
// target (int16 add/sub wraps the same way in scalar and vector code).

template<int L1>
inline void add_sub(int16_t* restrict_out, const int16_t* in, const int16_t* add, const int16_t* sub) {
    for (int i = 0; i < L1; ++i)
        restrict_out[i] = int16_t(in[i] + add[i] - sub[i]);
}

template<int L1>
inline void add_sub_sub(int16_t* out, const int16_t* in, const int16_t* add, const int16_t* sub1, const int16_t* sub2) {
    for (int i = 0; i < L1; ++i)
        out[i] = int16_t(in[i] + add[i] - sub1[i] - sub2[i]);
}

template<int L1>
inline void add_add_sub_sub(int16_t* out, const int16_t* in, const int16_t* add1, const int16_t* add2,
                            const int16_t* sub1, const int16_t* sub2) {
    for (int i = 0; i < L1; ++i)
        out[i] = int16_t(in[i] + add1[i] + add2[i] - sub1[i] - sub2[i]);
}

template<int L1>
inline void add_column(int16_t* acc, const int16_t* col) {
    for (int i = 0; i < L1; ++i)
        acc[i] = int16_t(acc[i] + col[i]);
}

template<int L1>
inline void sub_column(int16_t* acc, const int16_t* col) {
    for (int i = 0; i < L1; ++i)
        acc[i] = int16_t(acc[i] - col[i]);
}

// One move's worth of change, for one perspective.
template<int L1>
void apply_dirty(const Network<L1>& net, const DirtyPiece& dp, Color c, Square ksq,
                 const Accumulator<L1>& from, Accumulator<L1>& to) {
    int added[3], removed[3], na = 0, nr = 0;
    for (int i = 0; i < dp.count; ++i)
    {
        if (dp.from[i] != SQ_NONE)
            removed[nr++] = feature_index(c, dp.from[i], dp.pc[i], ksq);
        if (dp.to[i] != SQ_NONE)
            added[na++] = feature_index(c, dp.to[i], dp.pc[i], ksq);
    }

    const int16_t* in  = from.v[c];
    int16_t*       out = to.v[c];

    if (na == 1 && nr == 1)
        add_sub<L1>(out, in, net.ft_weights(added[0]), net.ft_weights(removed[0]));
    else if (na == 1 && nr == 2)
        add_sub_sub<L1>(out, in, net.ft_weights(added[0]), net.ft_weights(removed[0]), net.ft_weights(removed[1]));
    else if (na == 2 && nr == 2)
        add_add_sub_sub<L1>(out, in, net.ft_weights(added[0]), net.ft_weights(added[1]),
                            net.ft_weights(removed[0]), net.ft_weights(removed[1]));
    else
    {
        // Null move (nothing changed) or any other combination.
        std::memcpy(out, in, sizeof(int16_t) * L1);
        for (int i = 0; i < na; ++i)
            add_column<L1>(out, net.ft_weights(added[i]));
        for (int i = 0; i < nr; ++i)
            sub_column<L1>(out, net.ft_weights(removed[i]));
    }

    for (int b = 0; b < PSQTBuckets; ++b)
    {
        int32_t v = from.psqt[c][b];
        for (int i = 0; i < na; ++i)
            v += net.ft_psqt(added[i])[b];
        for (int i = 0; i < nr; ++i)
            v -= net.ft_psqt(removed[i])[b];
        to.psqt[c][b] = v;
    }
    to.computed[c] = true;
}

// Rebuilds one perspective from the refresh cache: bring the cached entry
// for this king square in line with the board, then copy it out.
template<int L1>
void refresh(const Position& pos, const Network<L1>& net, RefreshCache<L1>& cache, Color c, Accumulator<L1>& acc) {
    const Square      ksq   = pos.king_square(c);
    RefreshEntry<L1>& entry = cache.entries[ksq][c];

    for (Color col : {WHITE, BLACK})
        for (PieceType pt = PAWN; pt <= KING; ++pt)
        {
            const Piece    pc     = make_piece(col, pt);
            const Bitboard oldBB  = entry.byColor[col] & entry.byType[pt];
            const Bitboard newBB  = pos.pieces(col, pt);
            Bitboard       toRemove = oldBB & ~newBB;
            Bitboard       toAdd    = newBB & ~oldBB;

            // Pair an addition with a removal where possible: one pass
            // over the accumulator instead of two.
            while (toRemove && toAdd)
            {
                const int r = feature_index(c, pop_lsb(toRemove), pc, ksq);
                const int a = feature_index(c, pop_lsb(toAdd), pc, ksq);
                add_sub<L1>(entry.v, entry.v, net.ft_weights(a), net.ft_weights(r));
                for (int b = 0; b < PSQTBuckets; ++b)
                    entry.psqt[b] += net.ft_psqt(a)[b] - net.ft_psqt(r)[b];
            }
            while (toRemove)
            {
                const int r = feature_index(c, pop_lsb(toRemove), pc, ksq);
                sub_column<L1>(entry.v, net.ft_weights(r));
                for (int b = 0; b < PSQTBuckets; ++b)
                    entry.psqt[b] -= net.ft_psqt(r)[b];
            }
            while (toAdd)
            {
                const int a = feature_index(c, pop_lsb(toAdd), pc, ksq);
                add_column<L1>(entry.v, net.ft_weights(a));
                for (int b = 0; b < PSQTBuckets; ++b)
                    entry.psqt[b] += net.ft_psqt(a)[b];
            }
        }

    for (Color col : {WHITE, BLACK})
        entry.byColor[col] = pos.pieces(col);
    for (PieceType pt = PAWN; pt <= KING; ++pt)
        entry.byType[pt] = pos.pieces(pt);

    std::memcpy(acc.v[c], entry.v, sizeof(entry.v));
    std::memcpy(acc.psqt[c], entry.psqt, sizeof(entry.psqt));
    acc.computed[c] = true;
}

}  // namespace

template<int L1>
void RefreshCache<L1>::clear(const Network<L1>& net) {
    for (auto& perSquare : entries)
        for (auto& e : perSquare)
        {
            std::memcpy(e.v, net.ft_biases(), sizeof(e.v));
            std::memset(e.psqt, 0, sizeof(e.psqt));
            std::memset(e.byColor, 0, sizeof(e.byColor));
            std::memset(e.byType, 0, sizeof(e.byType));
        }
}

template<int L1>
void refresh_from_scratch(const Position& pos, const Network<L1>& net, Accumulator<L1>& acc) {
    for (Color c : {WHITE, BLACK})
    {
        const Square ksq = pos.king_square(c);
        std::memcpy(acc.v[c], net.ft_biases(), sizeof(int16_t) * L1);
        std::memset(acc.psqt[c], 0, sizeof(acc.psqt[c]));
        for (Bitboard b = pos.pieces(); b;)
        {
            const Square s   = pop_lsb(b);
            const int    idx = feature_index(c, s, pos.piece_on(s), ksq);
            add_column<L1>(acc.v[c], net.ft_weights(idx));
            for (int k = 0; k < PSQTBuckets; ++k)
                acc.psqt[c][k] += net.ft_psqt(idx)[k];
        }
        acc.computed[c] = true;
    }
}

template void refresh_from_scratch<L1Big>(const Position&, const Network<L1Big>&, Accumulator<L1Big>&);
template void refresh_from_scratch<L1Small>(const Position&, const Network<L1Small>&, Accumulator<L1Small>&);

AccumulatorStack::AccumulatorStack() {
    states     = static_cast<AccumulatorState*>(aligned_large_alloc(sizeof(AccumulatorState) * (MAX_PLY + 8)));
    bigCache   = static_cast<RefreshCache<L1Big>*>(aligned_large_alloc(sizeof(RefreshCache<L1Big>)));
    smallCache = static_cast<RefreshCache<L1Small>*>(aligned_large_alloc(sizeof(RefreshCache<L1Small>)));
    if (!states || !bigCache || !smallCache)
    {
        std::fputs("Nest: out of memory allocating evaluation state\n", stderr);
        std::abort();
    }
    reset();
}

AccumulatorStack::~AccumulatorStack() {
    aligned_large_free(states);
    aligned_large_free(bigCache);
    aligned_large_free(smallCache);
}

void AccumulatorStack::reset() {
    size = 1;
    states[0].dirty.count = 0;
    states[0].big.computed[WHITE] = states[0].big.computed[BLACK] = false;
    states[0].small.computed[WHITE] = states[0].small.computed[BLACK] = false;
    if (networks && networks->big.loaded())
        bigCache->clear(networks->big);
    if (networks && networks->small.loaded())
        smallCache->clear(networks->small);
}

template<int L1>
void AccumulatorStack::update(const Position& pos, const Network<L1>& net, RefreshCache<L1>& cache,
                              Accumulator<L1> AccumulatorState::* member, Color c) {
    const int   top  = size - 1;
    const Piece king = make_piece(c, KING);

    // Find the nearest ancestor whose accumulator is usable. A move of our
    // own king changes every feature, so nothing below it can be reused.
    int i = top;
    while (!(states[i].*member).computed[c])
    {
        if (i == 0 || (states[i].dirty.count && states[i].dirty.pc[0] == king))
        {
            refresh(pos, net, cache, c, states[top].*member);
            return;
        }
        --i;
    }

    const Square ksq = pos.king_square(c);
    for (int j = i + 1; j <= top; ++j)
        apply_dirty(net, states[j].dirty, c, ksq, states[j - 1].*member, states[j].*member);
}

#if !defined(NDEBUG)
namespace {
// Debug builds recompute every accumulator from scratch and compare.
template<int L1>
bool matches_scratch(const Position& pos, const Network<L1>& net, const Accumulator<L1>& acc) {
    auto* ref = static_cast<Accumulator<L1>*>(aligned_alloc_bytes(CacheLine, sizeof(Accumulator<L1>)));
    refresh_from_scratch(pos, net, *ref);
    const bool same = !std::memcmp(ref->v, acc.v, sizeof(acc.v)) && !std::memcmp(ref->psqt, acc.psqt, sizeof(acc.psqt));
    aligned_free(ref);
    return same;
}
}
#endif

const Accumulator<L1Big>& AccumulatorStack::big(const Position& pos) {
    update(pos, networks->big, *bigCache, &AccumulatorState::big, WHITE);
    update(pos, networks->big, *bigCache, &AccumulatorState::big, BLACK);
    assert(matches_scratch(pos, networks->big, states[size - 1].big));
    return states[size - 1].big;
}

const Accumulator<L1Small>& AccumulatorStack::small(const Position& pos) {
    update(pos, networks->small, *smallCache, &AccumulatorState::small, WHITE);
    update(pos, networks->small, *smallCache, &AccumulatorState::small, BLACK);
    assert(matches_scratch(pos, networks->small, states[size - 1].small));
    return states[size - 1].small;
}

}  // namespace Nest::NNUE
