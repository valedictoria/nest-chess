/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Nest {

using TimePoint = int64_t;  // milliseconds

inline TimePoint now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// xorshift64* generator; deterministic so that keys are the same everywhere.
class PRNG {
    uint64_t s;

   public:
    explicit PRNG(uint64_t seed) :
        s(seed) {}
    uint64_t next() {
        s ^= s >> 12, s ^= s << 25, s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
};

inline void prefetch(const void* addr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr);
#else
    (void) addr;
#endif
}

// High 64 bits of a 64x64 multiplication: maps a hash uniformly onto [0, n).
inline uint64_t mul_hi64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    __extension__ using uint128 = unsigned __int128;
    return uint64_t((uint128(a) * uint128(b)) >> 64);
#else
    const uint64_t aL = uint32_t(a), aH = a >> 32, bL = uint32_t(b), bH = b >> 32;
    const uint64_t c1 = (aL * bL) >> 32, c2 = aH * bL + c1, c3 = aL * bH + uint32_t(c2);
    return aH * bH + (c2 >> 32) + (c3 >> 32);
#endif
}

void* aligned_alloc_bytes(size_t alignment, size_t size);
void  aligned_free(void* ptr);
// Aligned allocation that asks for huge pages where the OS supports it.
void* aligned_large_alloc(size_t size);
void  aligned_large_free(void* ptr);

std::string engine_info();
std::string compiler_info();

}  // namespace Nest
