/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.

  Loader and forward pass for the Stockfish 17.1 network format; see nnue.h.
*/

#include <algorithm>
#include <cstring>

#include "../misc.h"
#include "nnue.h"
#include "simd.h"

namespace Nest::NNUE {

Networks* networks = nullptr;

namespace {

constexpr uint32_t FileVersion = 0x7AF32F20u;
constexpr char     Leb128Magic[] = "COMPRESSED_LEB128";
constexpr size_t   Leb128MagicSize = sizeof(Leb128Magic) - 1;

constexpr uint32_t affine_hash(uint32_t prev, uint32_t outputs) {
    uint32_t h = 0xCC03DAE4u + outputs;
    h ^= prev >> 1;
    h ^= prev << 31;
    return h;
}
constexpr uint32_t relu_hash(uint32_t prev) { return 0x538D24C7u + prev; }

constexpr uint32_t transformer_hash(int l1) { return 0x7f234cb8u ^ uint32_t(l1 * 2); }
constexpr uint32_t stack_hash(int l1) {
    uint32_t h = 0xEC42E90Du ^ uint32_t(l1 * 2);
    h          = affine_hash(h, L2 + 1);
    h          = relu_hash(h);
    h          = affine_hash(h, L3);
    h          = relu_hash(h);
    h          = affine_hash(h, 1);
    return h;
}

// Bounds-checked little-endian reader over a memory image.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool           ok = true;

    bool need(size_t n) {
        if (!ok || size_t(end - p) < n)
            ok = false;
        return ok;
    }
    uint32_t u32() {
        if (!need(4))
            return 0;
        const uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        p += 4;
        return v;
    }
    void bytes(void* dst, size_t n) {
        if (!need(n))
            return;
        std::memcpy(dst, p, n);
        p += n;
    }
    void i32s(int32_t* dst, size_t n) {
        for (size_t i = 0; i < n; ++i)
            dst[i] = int32_t(u32());
    }

    // A block of signed LEB128 integers: magic, byte count, data.
    template<typename T>
    void leb128(T* dst, size_t count) {
        if (!need(Leb128MagicSize) || std::memcmp(p, Leb128Magic, Leb128MagicSize) != 0)
        {
            ok = false;
            return;
        }
        p += Leb128MagicSize;
        const uint32_t byteCount = u32();
        if (!need(byteCount))
            return;
        const uint8_t* q    = p;
        const uint8_t* qend = p + byteCount;
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t result = 0;
            unsigned shift  = 0;
            while (true)
            {
                if (q == qend)
                {
                    ok = false;
                    return;
                }
                const uint8_t byte = *q++;
                result |= uint32_t(byte & 0x7f) << shift;
                shift += 7;
                if (!(byte & 0x80))
                {
                    if (shift < 32 && (byte & 0x40))
                        result |= ~uint32_t(0) << shift;
                    break;
                }
                if (shift >= 32)
                    break;
            }
            dst[i] = T(int32_t(result));
        }
        if (q != qend)
            ok = false;
        p = qend;
    }
};

// ---- sparse first layer helpers --------------------------------------------

struct NnzLookup {
    alignas(16) uint16_t idx[256][8];
    constexpr NnzLookup() :
        idx() {
        for (int m = 0; m < 256; ++m)
        {
            int n = 0;
            for (int b = 0; b < 8; ++b)
                if (m & (1 << b))
                    idx[m][n++] = uint16_t(b);
            for (; n < 8; ++n)
                idx[m][n] = 0;
        }
    }
};
constexpr NnzLookup Nnz;

// Writes the indices of the non-zero 4-byte chunks of `in` and returns how
// many there are. `out` needs room for chunks + 8 entries.
inline int find_nnz(const uint8_t* in, int chunks, uint16_t* out) {
    int count = 0;
#if defined(USE_AVX2)
    const __m256i zero = _mm256_setzero_si256();
    __m128i       base = _mm_setzero_si128();
    const __m128i inc  = _mm_set1_epi16(8);
    for (int i = 0; i < chunks; i += 8)
    {
        const __m256i  v = _mm256_load_si256(reinterpret_cast<const __m256i*>(in + 4 * i));
        const unsigned m =
          unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(v, zero))));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + count),
                         _mm_add_epi16(base, _mm_load_si128(reinterpret_cast<const __m128i*>(Nnz.idx[m]))));
        count += std::popcount(m);
        base = _mm_add_epi16(base, inc);
    }
#elif defined(USE_SSE2)
    const __m128i zero = _mm_setzero_si128();
    __m128i       base = _mm_setzero_si128();
    const __m128i inc  = _mm_set1_epi16(8);
    for (int i = 0; i < chunks; i += 8)
    {
        const __m128i  a = _mm_load_si128(reinterpret_cast<const __m128i*>(in + 4 * i));
        const __m128i  b = _mm_load_si128(reinterpret_cast<const __m128i*>(in + 4 * i + 16));
        const unsigned m = unsigned(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(a, zero))))
                         | unsigned(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(b, zero)))) << 4;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + count),
                         _mm_add_epi16(base, _mm_load_si128(reinterpret_cast<const __m128i*>(Nnz.idx[m]))));
        count += std::popcount(m);
        base = _mm_add_epi16(base, inc);
    }
#elif defined(USE_NEON)
    static const uint32_t Bits[4] = {1, 2, 4, 8};
    const uint32x4_t      bits    = vld1q_u32(Bits);
    uint16x8_t            base    = vdupq_n_u16(0);
    const uint16x8_t      inc     = vdupq_n_u16(8);
    for (int i = 0; i < chunks; i += 8)
    {
        const uint32x4_t a = vreinterpretq_u32_u8(vld1q_u8(in + 4 * i));
        const uint32x4_t b = vreinterpretq_u32_u8(vld1q_u8(in + 4 * i + 16));
        const unsigned   m = vaddvq_u32(vandq_u32(vtstq_u32(a, a), bits))
                           | vaddvq_u32(vandq_u32(vtstq_u32(b, b), bits)) << 4;
        vst1q_u16(out + count, vaddq_u16(base, vld1q_u16(Nnz.idx[m])));
        count += std::popcount(m);
        base = vaddq_u16(base, inc);
    }
#else
    for (int i = 0; i < chunks; ++i)
    {
        uint32_t v;
        std::memcpy(&v, in + 4 * i, 4);
        out[count] = uint16_t(i);
        count += v != 0;
    }
#endif
    return count;
}

// out[o] += sum over the listed 4-byte input chunks of in[j] * w[j][o].
// Weights are laid out [chunk][output][4], so that one chunk's contribution
// to all outputs is a single run of memory.
template<int Outputs>
inline void affine_chunks(int32_t* out, const int8_t* w, const uint8_t* in, const uint16_t* idx, int count) {
#if defined(USE_AVX512)
    constexpr int R = Outputs / 16;
    __m512i       acc[R];
    for (int r = 0; r < R; ++r)
        acc[r] = _mm512_load_si512(out + 16 * r);
    for (int k = 0; k < count; ++k)
    {
        const int i = idx[k];
        uint32_t  v;
        std::memcpy(&v, in + 4 * i, 4);
        const __m512i  in32 = _mm512_set1_epi32(int(v));
        const __m512i* col  = reinterpret_cast<const __m512i*>(w + size_t(i) * Outputs * 4);
        for (int r = 0; r < R; ++r)
        {
    #if defined(USE_VNNI)
            acc[r] = _mm512_dpbusd_epi32(acc[r], in32, _mm512_load_si512(col + r));
    #else
            acc[r] = _mm512_add_epi32(
              acc[r], _mm512_madd_epi16(_mm512_maddubs_epi16(in32, _mm512_load_si512(col + r)),
                                        _mm512_set1_epi16(1)));
    #endif
        }
    }
    for (int r = 0; r < R; ++r)
        _mm512_store_si512(out + 16 * r, acc[r]);
#elif defined(USE_AVX2)
    constexpr int R = Outputs / 8;
    __m256i       acc[R];
    for (int r = 0; r < R; ++r)
        acc[r] = _mm256_load_si256(reinterpret_cast<const __m256i*>(out + 8 * r));
    const __m256i ones = _mm256_set1_epi16(1);
    for (int k = 0; k < count; ++k)
    {
        const int i = idx[k];
        uint32_t  v;
        std::memcpy(&v, in + 4 * i, 4);
        const __m256i  in32 = _mm256_set1_epi32(int(v));
        const __m256i* col  = reinterpret_cast<const __m256i*>(w + size_t(i) * Outputs * 4);
        for (int r = 0; r < R; ++r)
            acc[r] = _mm256_add_epi32(
              acc[r], _mm256_madd_epi16(_mm256_maddubs_epi16(in32, _mm256_load_si256(col + r)), ones));
    }
    for (int r = 0; r < R; ++r)
        _mm256_store_si256(reinterpret_cast<__m256i*>(out + 8 * r), acc[r]);
#elif defined(USE_SSSE3)
    constexpr int R = Outputs / 4;
    __m128i       acc[R];
    for (int r = 0; r < R; ++r)
        acc[r] = _mm_load_si128(reinterpret_cast<const __m128i*>(out + 4 * r));
    const __m128i ones = _mm_set1_epi16(1);
    for (int k = 0; k < count; ++k)
    {
        const int i = idx[k];
        uint32_t  v;
        std::memcpy(&v, in + 4 * i, 4);
        const __m128i  in32 = _mm_set1_epi32(int(v));
        const __m128i* col  = reinterpret_cast<const __m128i*>(w + size_t(i) * Outputs * 4);
        for (int r = 0; r < R; ++r)
            acc[r] = _mm_add_epi32(
              acc[r], _mm_madd_epi16(_mm_maddubs_epi16(in32, _mm_load_si128(col + r)), ones));
    }
    for (int r = 0; r < R; ++r)
        _mm_store_si128(reinterpret_cast<__m128i*>(out + 4 * r), acc[r]);
#elif defined(USE_NEON)
    constexpr int R = Outputs / 4;
    int32x4_t     acc[R];
    for (int r = 0; r < R; ++r)
        acc[r] = vld1q_s32(out + 4 * r);
    for (int k = 0; k < count; ++k)
    {
        const int i = idx[k];
        uint32_t  v;
        std::memcpy(&v, in + 4 * i, 4);
        // Inputs are at most 127, so they are also valid signed bytes.
        const int8x16_t in8 = vreinterpretq_s8_u32(vdupq_n_u32(v));
        const int8_t*   col = w + size_t(i) * Outputs * 4;
        for (int r = 0; r < R; ++r)
        {
            const int8x16_t wv = vld1q_s8(col + 16 * r);
    #if defined(USE_NEON_DOTPROD)
            acc[r] = vdotq_s32(acc[r], in8, wv);
    #else
            const int16x8_t lo = vmull_s8(vget_low_s8(in8), vget_low_s8(wv));
            const int16x8_t hi = vmull_high_s8(in8, wv);
            acc[r]             = vpadalq_s16(acc[r], vpaddq_s16(lo, hi));
    #endif
        }
    }
    for (int r = 0; r < R; ++r)
        vst1q_s32(out + 4 * r, acc[r]);
#else
    for (int k = 0; k < count; ++k)
    {
        const int     i   = idx[k];
        const int8_t* col = w + size_t(i) * Outputs * 4;
        for (int o = 0; o < Outputs; ++o)
            for (int j = 0; j < 4; ++j)
                out[o] += int(in[4 * i + j]) * int(col[4 * o + j]);
    }
#endif
}

}  // namespace

template<int L1>
Network<L1>::Network() {
    biases      = static_cast<int16_t*>(aligned_alloc_bytes(CacheLine, sizeof(int16_t) * L1));
    weights     = static_cast<int16_t*>(aligned_large_alloc(sizeof(int16_t) * size_t(InputDims) * L1));
    psqtWeights = static_cast<int32_t*>(aligned_large_alloc(sizeof(int32_t) * size_t(InputDims) * PSQTBuckets));
    stacks      = static_cast<LayerStack<L1>*>(aligned_alloc_bytes(CacheLine, sizeof(LayerStack<L1>) * LayerStacks));
}

template<int L1>
Network<L1>::~Network() {
    aligned_free(biases);
    aligned_large_free(weights);
    aligned_large_free(psqtWeights);
    aligned_free(stacks);
}

template<int L1>
bool Network<L1>::load(const uint8_t* data, size_t size, std::string& error) {
    isLoaded = false;
    if (!biases || !weights || !psqtWeights || !stacks)
    {
        error = "out of memory";
        return false;
    }

    Reader r{data, data + size};

    const uint32_t version  = r.u32();
    const uint32_t hash     = r.u32();
    const uint32_t descSize = r.u32();
    if (!r.ok || version != FileVersion)
    {
        error = "not a network file of the supported version";
        return false;
    }
    if (hash != (transformer_hash(L1) ^ stack_hash(L1)))
    {
        error = "network architecture does not match (expected HalfKAv2_hm " + std::to_string(L1)
              + "x2-15-32-1 of Stockfish 17.1)";
        return false;
    }
    if (!r.need(descSize))
    {
        error = "file is truncated";
        return false;
    }
    description.assign(reinterpret_cast<const char*>(r.p), descSize);
    r.p += descSize;

    if (r.u32() != transformer_hash(L1))
    {
        error = "feature transformer section is not the expected one";
        return false;
    }
    r.leb128(biases, L1);
    r.leb128(weights, size_t(InputDims) * L1);
    r.leb128(psqtWeights, size_t(InputDims) * PSQTBuckets);
    if (!r.ok)
    {
        error = "feature transformer data is corrupt or truncated";
        return false;
    }

    // Stored doubled: the pairwise product in transform() then needs a
    // shift by 9, which fits the multiply-high instructions.
    for (int i = 0; i < L1; ++i)
        biases[i] = int16_t(biases[i] * 2);
    for (size_t i = 0; i < size_t(InputDims) * L1; ++i)
        weights[i] = int16_t(weights[i] * 2);

    for (int s = 0; s < LayerStacks; ++s)
    {
        LayerStack<L1>& ls = stacks[s];
        std::memset(static_cast<void*>(&ls), 0, sizeof(ls));

        if (r.u32() != stack_hash(L1))
        {
            error = "layer stack section is not the expected one";
            return false;
        }

        // File order is [output][input]; we store [input / 4][output][input % 4].
        r.i32s(ls.b0, L2 + 1);
        for (int o = 0; o < L2 + 1 && r.need(L1); ++o, r.p += L1)
            for (int j = 0; j < L1; ++j)
                ls.w0[(j / 4) * (L2 + 1) * 4 + o * 4 + j % 4] = int8_t(r.p[j]);

        r.i32s(ls.b1, L3);
        for (int o = 0; o < L3 && r.need(32); ++o, r.p += 32)
            for (int j = 0; j < 32; ++j)
                ls.w1[(j / 4) * L3 * 4 + o * 4 + j % 4] = int8_t(r.p[j]);

        r.i32s(&ls.b2, 1);
        r.bytes(ls.w2, L3);
    }

    if (!r.ok || r.p != r.end)
    {
        error = "layer data is truncated or has trailing bytes";
        return false;
    }

    isLoaded = true;
    return true;
}

// Clipped pairwise product of the two halves of each perspective's
// accumulator: out = clamp(a, 0, 254) * clamp(b, 0, 254) / 512, in 0..126.
template<int L1>
void Network<L1>::transform(const Accumulator<L1>& acc, Color stm, uint8_t* out) const {
    constexpr int Half = L1 / 2;
    const Color   perspectives[2] = {stm, ~stm};

    for (int p = 0; p < 2; ++p)
    {
        const int16_t* in0 = acc.v[perspectives[p]];
        const int16_t* in1 = in0 + Half;
        uint8_t*       o   = out + Half * p;

#if defined(USE_AVX2)
        const __m256i zero = _mm256_setzero_si256();
        const __m256i one  = _mm256_set1_epi16(254);
        for (int j = 0; j < Half; j += 32)
        {
            const __m256i a0 = _mm256_slli_epi16(
              _mm256_max_epi16(_mm256_min_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(in0 + j)), one), zero), 7);
            const __m256i a1 = _mm256_slli_epi16(
              _mm256_max_epi16(_mm256_min_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(in0 + j + 16)), one), zero), 7);
            const __m256i b0 = _mm256_min_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(in1 + j)), one);
            const __m256i b1 = _mm256_min_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(in1 + j + 16)), one);
            // A negative b gives a negative product, which the unsigned
            // saturating pack turns into 0: same as clamping b first.
            const __m256i packed = _mm256_packus_epi16(_mm256_mulhi_epi16(a0, b0), _mm256_mulhi_epi16(a1, b1));
            _mm256_store_si256(reinterpret_cast<__m256i*>(o + j), _mm256_permute4x64_epi64(packed, 0xD8));
        }
#elif defined(USE_SSE2)
        const __m128i zero = _mm_setzero_si128();
        const __m128i one  = _mm_set1_epi16(254);
        for (int j = 0; j < Half; j += 16)
        {
            const __m128i a0 = _mm_slli_epi16(
              _mm_max_epi16(_mm_min_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(in0 + j)), one), zero), 7);
            const __m128i a1 = _mm_slli_epi16(
              _mm_max_epi16(_mm_min_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(in0 + j + 8)), one), zero), 7);
            const __m128i b0 = _mm_min_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(in1 + j)), one);
            const __m128i b1 = _mm_min_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(in1 + j + 8)), one);
            _mm_store_si128(reinterpret_cast<__m128i*>(o + j),
                            _mm_packus_epi16(_mm_mulhi_epi16(a0, b0), _mm_mulhi_epi16(a1, b1)));
        }
#elif defined(USE_NEON)
        const int16x8_t zero = vdupq_n_s16(0);
        const int16x8_t one  = vdupq_n_s16(254);
        for (int j = 0; j < Half; j += 16)
        {
            // vqdmulh doubles the product, so shift by 6 instead of 7.
            const int16x8_t a0 = vshlq_n_s16(vmaxq_s16(vminq_s16(vld1q_s16(in0 + j), one), zero), 6);
            const int16x8_t a1 = vshlq_n_s16(vmaxq_s16(vminq_s16(vld1q_s16(in0 + j + 8), one), zero), 6);
            const int16x8_t b0 = vminq_s16(vld1q_s16(in1 + j), one);
            const int16x8_t b1 = vminq_s16(vld1q_s16(in1 + j + 8), one);
            vst1q_u8(o + j, vcombine_u8(vqmovun_s16(vqdmulhq_s16(a0, b0)), vqmovun_s16(vqdmulhq_s16(a1, b1))));
        }
#else
        for (int j = 0; j < Half; ++j)
        {
            const int a = std::clamp<int>(in0[j], 0, 254);
            const int b = std::clamp<int>(in1[j], 0, 254);
            o[j]        = uint8_t(unsigned(a * b) / 512);
        }
#endif
    }
}

template<int L1>
void Network<L1>::evaluate(const Accumulator<L1>& acc, Color stm, int bucket, int& psqt, int& positional) const {
    alignas(CacheLine) uint8_t  ft[L1];
    alignas(CacheLine) int32_t  o0[L2 + 1];
    alignas(CacheLine) uint8_t  in1[32];
    alignas(CacheLine) int32_t  o1[L3];
    uint16_t                    nnz[L1 / 4 + 16];

    transform(acc, stm, ft);

    const LayerStack<L1>& ls = stacks[bucket];

    // First layer: most inputs are zero, so only non-zero chunks are visited.
    std::memcpy(o0, ls.b0, sizeof(o0));
    const int count = find_nnz(ft, L1 / 4, nnz);
    affine_chunks<L2 + 1>(o0, ls.w0, ft, nnz, count);

    // Two activations of the first 15 outputs, concatenated: squared and
    // plain clipped ReLU. Entries 30 and 31 are padding.
    for (int i = 0; i < L2; ++i)
    {
        in1[i]      = uint8_t(std::min<long long>(127, (static_cast<long long>(o0[i]) * o0[i]) >> (2 * WeightScaleBits + 7)));
        in1[L2 + i] = uint8_t(std::clamp(o0[i] >> WeightScaleBits, 0, 127));
    }
    in1[30] = in1[31] = 0;

    static constexpr uint16_t AllChunks[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    std::memcpy(o1, ls.b1, sizeof(o1));
    affine_chunks<L3>(o1, ls.w1, in1, AllChunks, 8);

    int32_t o2 = ls.b2;
    for (int i = 0; i < L3; ++i)
        o2 += std::clamp(o1[i] >> WeightScaleBits, 0, 127) * int(ls.w2[i]);

    // The 16th output of the first layer skips the rest of the stack.
    const int32_t fwd = o0[L2] * (600 * OutputScale) / (127 * (1 << WeightScaleBits));

    positional = (o2 + fwd) / OutputScale;
    psqt       = ((acc.psqt[stm][bucket] - acc.psqt[~stm][bucket]) / 2) / OutputScale;
}

template class Network<L1Big>;
template class Network<L1Small>;

}  // namespace Nest::NNUE
