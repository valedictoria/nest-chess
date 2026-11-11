// Finds fancy-magic multipliers for rook and bishop attacks and prints them
// as src/magics.inc. Run once; the output is deterministic.
//   g++ -O2 -std=c++20 tools/gen_magics.cpp -o gen_magics && ./gen_magics > src/magics.inc
#include <bit>
#include <cstdint>
#include <cstdio>
#include <vector>

using u64 = uint64_t;

static u64 slide(int sq, u64 occ, const int dirs[4][2]) {
    u64 a = 0;
    for (int d = 0; d < 4; ++d) {
        int f = sq % 8 + dirs[d][0], r = sq / 8 + dirs[d][1];
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            a |= 1ULL << (r * 8 + f);
            if (occ & (1ULL << (r * 8 + f)))
                break;
            f += dirs[d][0], r += dirs[d][1];
        }
    }
    return a;
}

static u64 s = 0x9E3779B97F4A7C15ULL;
static u64 rnd() {
    s ^= s >> 12, s ^= s << 25, s ^= s >> 27;
    return s * 2685821657736338717ULL;
}

int main() {
    const int rookDirs[4][2]   = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    const int bishopDirs[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    const char* names[2]       = {"RookMagicNumbers", "BishopMagicNumbers"};

    for (int pt = 0; pt < 2; ++pt) {
        const auto& dirs = pt == 0 ? rookDirs : bishopDirs;
        std::printf("constexpr Bitboard %s[SQUARE_NB] = {\n", names[pt]);
        for (int sq = 0; sq < 64; ++sq) {
            u64 edges = ((0xFF000000000000FFULL) & ~(0xFFULL << (8 * (sq / 8))))
                      | ((0x8181818181818181ULL) & ~(0x0101010101010101ULL << (sq % 8)));
            u64 mask = slide(sq, 0, dirs) & ~edges;
            int bits = std::popcount(mask);
            std::vector<u64> occs, atts;
            u64 b = 0;
            do {
                occs.push_back(b);
                atts.push_back(slide(sq, b, dirs));
                b = (b - mask) & mask;
            } while (b);

            std::vector<u64> table(1ULL << bits);
            std::vector<int> epoch(1ULL << bits, 0);
            int e = 0;
            u64 magic;
            for (;;) {
                do
                    magic = rnd() & rnd() & rnd();
                while (std::popcount((magic * mask) >> 56) < 6);
                ++e;
                bool ok = true;
                for (size_t i = 0; i < occs.size() && ok; ++i) {
                    u64 idx = (occs[i] * magic) >> (64 - bits);
                    if (epoch[idx] < e)
                        epoch[idx] = e, table[idx] = atts[i];
                    else if (table[idx] != atts[i])
                        ok = false;
                }
                if (ok)
                    break;
            }
            std::printf("  0x%016llxULL,%s", (unsigned long long) magic, sq % 4 == 3 ? "\n" : "");
        }
        std::printf("};\n\n");
    }
}
