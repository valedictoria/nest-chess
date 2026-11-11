/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "tt.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

namespace Nest {

void TTEntry::save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev, uint8_t generation8) {
    // Keep the old move when the new result has none for the same position.
    if (m != Move::none() || uint16_t(k) != key16)
        move16 = m;

    // Overwrite less valuable entries: exact bounds, other positions, and
    // results that are not much shallower than what is stored.
    if (b == BOUND_EXACT || uint16_t(k) != key16 || d - DEPTH_ENTRY_OFFSET + 2 * pv > depth8 - 4)
    {
        key16     = uint16_t(k);
        depth8    = uint8_t(d - DEPTH_ENTRY_OFFSET);
        genBound8 = uint8_t(generation8 | uint8_t(pv) << 2 | b);
        value16   = int16_t(v);
        eval16    = int16_t(ev);
    }
}

bool TranspositionTable::resize(size_t megabytes, size_t threads) {
    aligned_large_free(table);
    clusterCount = megabytes * 1024 * 1024 / sizeof(Cluster);
    table        = static_cast<Cluster*>(aligned_large_alloc(clusterCount * sizeof(Cluster)));
    if (!table)
    {
        clusterCount = 0;
        return false;
    }
    clear(threads);
    return true;
}

void TranspositionTable::clear(size_t threads) {
    generation8 = 0;
    if (!table)
        return;

    threads = std::clamp<size_t>(threads, 1, 64);
    if (clusterCount < 1024 * threads)
        threads = 1;

    std::vector<std::thread> pool;
    for (size_t i = 0; i < threads; ++i)
        pool.emplace_back([this, i, threads] {
            const size_t stride = clusterCount / threads;
            const size_t start  = stride * i;
            const size_t len    = i + 1 == threads ? clusterCount - start : stride;
            std::memset(static_cast<void*>(&table[start]), 0, len * sizeof(Cluster));
        });
    for (auto& th : pool)
        th.join();
}

TTEntry* TranspositionTable::probe(Key key, bool& found, TTData& data) const {
    TTEntry* const tte   = table[mul_hi64(key, clusterCount)].entry;
    const uint16_t key16 = uint16_t(key);

    for (int i = 0; i < ClusterSize; ++i)
        if (tte[i].key16 == key16 && tte[i].depth8)
        {
            // Refresh the age so that the entry survives this search.
            tte[i].genBound8 = uint8_t(generation8 | (tte[i].genBound8 & (GenerationDelta - 1)));

            found      = true;
            data.move  = tte[i].move16;
            data.value = tte[i].value16;
            data.eval  = tte[i].eval16;
            data.depth = Depth(tte[i].depth8) + DEPTH_ENTRY_OFFSET;
            data.bound = Bound(tte[i].genBound8 & 0x3);
            data.isPv  = bool(tte[i].genBound8 & 0x4);
            return &tte[i];
        }

    // Not there: pick the slot to overwrite, the least valuable by depth and age.
    auto worth = [this](const TTEntry& e) {
        const int age = (GenerationCycle + generation8 - e.genBound8) & GenerationMask;
        return e.depth8 - age;
    };
    TTEntry* replace = tte;
    for (int i = 1; i < ClusterSize; ++i)
        if (worth(*replace) > worth(tte[i]))
            replace = &tte[i];

    found      = false;
    data.move  = Move::none();
    data.value = data.eval = VALUE_NONE;
    data.depth             = DEPTH_ENTRY_OFFSET;
    data.bound             = BOUND_NONE;
    data.isPv              = false;
    return replace;
}

int TranspositionTable::hashfull() const {
    if (clusterCount < 1000)
        return 0;
    int cnt = 0;
    for (int i = 0; i < 1000; ++i)
        for (int j = 0; j < ClusterSize; ++j)
            cnt += table[i].entry[j].depth8
                && (table[i].entry[j].genBound8 & GenerationMask) == generation8;
    return cnt / ClusterSize;
}

}  // namespace Nest
