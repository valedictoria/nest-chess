/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "misc.h"

#include <cstdlib>
#include <sstream>

#if defined(__linux__)
    #include <sys/mman.h>
#endif
#if defined(_WIN32)
    #include <malloc.h>
#endif

namespace Nest {

void* aligned_alloc_bytes(size_t alignment, size_t size) {
    size = (size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
#endif
}

void aligned_free(void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void* aligned_large_alloc(size_t size) {
#if defined(__linux__)
    constexpr size_t HugePage = 2 * 1024 * 1024;
    void* p = aligned_alloc_bytes(size >= HugePage ? HugePage : 4096, size);
    #if defined(MADV_HUGEPAGE)
    if (p && size >= HugePage)
        madvise(p, (size + HugePage - 1) / HugePage * HugePage, MADV_HUGEPAGE);
    #endif
    return p;
#else
    // macOS has no transparent huge pages; Windows large pages need a
    // privilege most users lack. Plain aligned memory on both.
    return aligned_alloc_bytes(4096, size);
#endif
}

void aligned_large_free(void* ptr) { aligned_free(ptr); }

#ifndef NEST_VERSION
    #define NEST_VERSION "dev"
#endif

std::string engine_info() { return std::string("Nest ") + NEST_VERSION; }

std::string compiler_info() {
    std::ostringstream ss;
#if defined(__clang__)
    ss << "clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
#elif defined(__GNUC__)
    ss << "gcc " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
#else
    ss << "unknown compiler";
#endif
    ss << " |";
#if defined(USE_AVX512)
    ss << " AVX512";
#endif
#if defined(USE_VNNI)
    ss << " VNNI";
#endif
#if defined(USE_AVX2)
    ss << " AVX2";
#endif
#if defined(USE_SSE41)
    ss << " SSE41";
#endif
#if defined(USE_SSSE3)
    ss << " SSSE3";
#endif
#if defined(USE_SSE2)
    ss << " SSE2";
#endif
#if defined(USE_POPCNT)
    ss << " POPCNT";
#endif
#if defined(USE_PEXT)
    ss << " BMI2";
#endif
#if defined(USE_NEON_DOTPROD)
    ss << " NEON_DOTPROD";
#elif defined(USE_NEON)
    ss << " NEON";
#endif
#if !defined(NDEBUG)
    ss << " DEBUG";
#endif
    return ss.str();
}

}  // namespace Nest
