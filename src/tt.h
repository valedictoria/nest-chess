/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "misc.h"
#include "types.h"

namespace Nest {

// What a probe returns: a private copy of the entry's contents.
struct TTData {
    Move  move;
    Value value, eval;
    Depth depth;
    Bound bound;
    bool  isPv;
};

// 10 bytes per entry:
//   key16      low 16 bits of the position key (the high bits pick the cluster)
//   depth8     depth - DEPTH_ENTRY_OFFSET; 0 marks an empty slot
//   genBound8  generation (5 bits) | pv flag (1 bit) | bound (2 bits)
//   move16, value16, eval16
struct TTEntry {
    void save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev, uint8_t generation8);

   private:
    friend class TranspositionTable;

    uint16_t key16;
    uint8_t  depth8;
    uint8_t  genBound8;
    Move     move16;
    int16_t  value16;
    int16_t  eval16;
};

// The table is shared by all search threads and is deliberately lock-free.
// Two threads may write the same entry at once and a reader may see a mix
// of both ("torn" entry). That is tolerated instead of prevented, because a
// lock (or even an atomic exchange) per probe costs far more than the rare
// bad entry: the 16-bit key check rejects almost all of them, the stored
// move is verified with Position::pseudo_legal()/legal() before it is played,
// and a wrong score or bound can only cost search efficiency, never memory
// safety. Entries are plain integers, so on the supported platforms a racy
// read simply yields some mix of old and new bytes.
class TranspositionTable {
    static constexpr int ClusterSize = 3;

    struct Cluster {
        TTEntry entry[ClusterSize];
        char    padding[2];
    };
    static_assert(sizeof(Cluster) == 32, "a cluster must be half a cache line");

   public:
    static constexpr unsigned GenerationBits  = 3;                        // bits below the generation
    static constexpr int      GenerationDelta = 1 << GenerationBits;      // one search = +8
    static constexpr int      GenerationCycle = 255 + GenerationDelta;
    static constexpr int      GenerationMask  = (0xFF << GenerationBits) & 0xFF;

    ~TranspositionTable() { aligned_large_free(table); }

    // Both use up to `threads` threads; clearing a large table is slow.
    bool resize(size_t megabytes, size_t threads);
    void clear(size_t threads);

    void    new_search() { generation8 = uint8_t(generation8 + GenerationDelta); }
    uint8_t generation() const { return generation8; }

    // Looks up the position. `found` tells whether `data` is meaningful; the
    // returned entry is where a result for this position should be saved.
    TTEntry* probe(Key key, bool& found, TTData& data) const;

    void prefetch(Key key) const { Nest::prefetch(&table[mul_hi64(key, clusterCount)]); }

    // Approximate fill level in per mille, counting entries of this search.
    int hashfull() const;

   private:
    Cluster* table        = nullptr;
    size_t   clusterCount = 0;
    uint8_t  generation8  = 0;
};

}  // namespace Nest
