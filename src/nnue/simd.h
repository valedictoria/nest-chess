/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

// Instruction-set selection, derived from what the compiler was told to
// target (-march / -mcpu), so that the Makefile only has to pick flags.
// Every vector path computes exactly what the scalar code computes.

#if defined(NEST_NO_SIMD)
// scalar reference build
#elif defined(__AVX512F__) && defined(__AVX512BW__)
    #define USE_AVX512
    #define USE_AVX2
    #define USE_SSSE3
    #define USE_SSE2
    #if defined(__AVX512VNNI__)
        #define USE_VNNI
    #endif
#elif defined(__AVX2__)
    #define USE_AVX2
    #define USE_SSSE3
    #define USE_SSE2
#elif defined(__SSSE3__)
    #define USE_SSSE3
    #define USE_SSE2
#elif defined(__SSE2__)
    #define USE_SSE2
#elif defined(__ARM_NEON) && defined(__aarch64__)
    #define USE_NEON
    #if defined(__ARM_FEATURE_DOTPROD)
        #define USE_NEON_DOTPROD
    #endif
#endif

#if defined(__SSE4_1__) && !defined(NEST_NO_SIMD)
    #define USE_SSE41
#endif
#if defined(__POPCNT__)
    #define USE_POPCNT
#endif

#if defined(USE_SSE2)
    #include <immintrin.h>
#elif defined(USE_NEON)
    #include <arm_neon.h>
#endif
