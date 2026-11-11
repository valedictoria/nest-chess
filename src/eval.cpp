/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.

  The way the two networks are selected and their outputs blended follows
  Stockfish 17.1 (src/evaluate.cpp), whose networks these are; the win-rate
  model constants come from the same release (src/uci.cpp).
*/

#include "eval.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

#include "bitboard.h"
#include "position.h"

// ---- embedded network images ------------------------------------------------
// The Makefile passes the file names when the nets are present at build time.
#if defined(NEST_EMBED_BIG) && defined(NEST_EMBED_SMALL) && !defined(_MSC_VER)
    #if defined(__APPLE__)
        #define NEST_ASM_SECTION ".section __DATA,__const\n"
        #define NEST_ASM_RESTORE ".text\n"
        #define NEST_ASM_SYM(n) "_" #n
    #elif defined(_WIN32)
        #define NEST_ASM_SECTION ".section .rdata,\"dr\"\n"
        #define NEST_ASM_RESTORE ".text\n"
        #define NEST_ASM_SYM(n) #n
    #else
        #define NEST_ASM_SECTION ".pushsection .rodata\n"
        #define NEST_ASM_RESTORE ".popsection\n"
        #define NEST_ASM_SYM(n) #n
    #endif
    #define NEST_INCBIN(name, file) \
        __asm__(NEST_ASM_SECTION ".balign 64\n" \
                ".globl " NEST_ASM_SYM(name) "\n" NEST_ASM_SYM(name) ":\n" \
                ".incbin \"" file "\"\n" \
                ".globl " NEST_ASM_SYM(name##End) "\n" NEST_ASM_SYM(name##End) ":\n" \
                ".byte 0\n" NEST_ASM_RESTORE); \
        extern "C" const unsigned char name[]; \
        extern "C" const unsigned char name##End[];

NEST_INCBIN(gNestBigNet, NEST_EMBED_BIG)
NEST_INCBIN(gNestSmallNet, NEST_EMBED_SMALL)
    #define NEST_HAS_EMBEDDED_NETS
#endif

namespace Nest {

namespace {

// ---- fallback evaluation ------------------------------------------------------
// Used only when no network could be loaded: material and generated
// piece-square terms. It keeps the engine usable, nothing more.
constexpr int Material[PIECE_TYPE_NB] = {0, 100, 320, 330, 500, 900, 0, 0};
constexpr int Phase[PIECE_TYPE_NB]    = {0, 0, 1, 1, 2, 4, 0, 0};

int centre_distance(Square s) {
    const int f = file_of(s), r = rank_of(s);
    return std::max(f < 4 ? 3 - f : f - 4, r < 4 ? 3 - r : r - 4);
}

void psq(PieceType pt, Square s, int& mg, int& eg) {
    const int r = rank_of(s), f = file_of(s), cd = centre_distance(s);
    switch (pt)
    {
    case PAWN :
        mg = 4 * (r - 1) + ((f == 3 || f == 4) && r >= 2 && r <= 4 ? 16 : 0);
        eg = 10 * (r - 1);
        break;
    case KNIGHT :
        mg = eg = 24 - 14 * cd;
        break;
    case BISHOP :
        mg = eg = 12 - 6 * cd;
        break;
    case ROOK :
        mg = (r == 6 ? 16 : 0) + (f == 3 || f == 4 ? 6 : 0);
        eg = r == 6 ? 10 : 0;
        break;
    case QUEEN :
        mg = 4 - 3 * cd;
        eg = 8 - 6 * cd;
        break;
    case KING :
        mg = -12 * r + (f <= 2 || f >= 6 ? 18 : -10);
        eg = 30 - 14 * cd;
        break;
    default :
        mg = eg = 0;
    }
}

Value classical(const Position& pos) {
    int mg = 0, eg = 0, phase = 0;
    for (Bitboard b = pos.pieces(); b;)
    {
        const Square    s    = pop_lsb(b);
        const Piece     pc   = pos.piece_on(s);
        const PieceType pt   = type_of(pc);
        const int       sign = color_of(pc) == WHITE ? 1 : -1;
        int             m, e;
        psq(pt, relative_square(color_of(pc), s), m, e);
        mg += sign * (Material[pt] + m);
        eg += sign * (Material[pt] + e);
        phase += Phase[pt];
    }
    phase       = std::min(phase, 24);
    const int v = (mg * phase + eg * (24 - phase)) / 24;
    // Roughly the scale of the network output (about 2 units per centipawn).
    return 2 * ((pos.side_to_move() == WHITE ? v : -v) + 10);
}

// ---- network selection and blending ---------------------------------------

int simple_eval(const Position& pos, Color c) {
    return PawnValue * (pos.count(c, PAWN) - pos.count(~c, PAWN))
         + (pos.non_pawn_material(c) - pos.non_pawn_material(~c));
}

int layer_bucket(const Position& pos) { return (popcount(pos.pieces()) - 1) / 4; }

void eval_big(const Position& pos, AccumulatorStack& acc, int& psqt, int& positional) {
    NNUE::networks->big.evaluate(acc.big(pos), pos.side_to_move(), layer_bucket(pos), psqt, positional);
}
void eval_small(const Position& pos, AccumulatorStack& acc, int& psqt, int& positional) {
    NNUE::networks->small.evaluate(acc.small(pos), pos.side_to_move(), layer_bucket(pos), psqt, positional);
}

struct WinRateParams {
    double a, b;
};

WinRateParams win_rate_params(const Position& pos) {
    const int material = pos.count(W_PAWN) + pos.count(B_PAWN)
                       + 3 * (pos.count(W_KNIGHT) + pos.count(B_KNIGHT))
                       + 3 * (pos.count(W_BISHOP) + pos.count(B_BISHOP))
                       + 5 * (pos.count(W_ROOK) + pos.count(B_ROOK))
                       + 9 * (pos.count(W_QUEEN) + pos.count(B_QUEEN));
    const double m = std::clamp(material, 17, 78) / 58.0;

    constexpr double as[] = {-13.50030198, 40.92780883, -36.82753545, 386.83004070};
    constexpr double bs[] = {96.53354896, -165.79058388, 90.89679019, 49.29561889};
    return {(((as[0] * m + as[1]) * m + as[2]) * m) + as[3],
            (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3]};
}

std::unique_ptr<NNUE::Networks> ownedNetworks;

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const std::streamoff size = f.tellg();
    if (size <= 0)
        return false;
    out.resize(size_t(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return bool(f);
}

void ensure_allocated() {
    if (!ownedNetworks)
    {
        ownedNetworks  = std::make_unique<NNUE::Networks>();
        NNUE::networks = ownedNetworks.get();
    }
}

}  // namespace

bool Eval::networks_ready() {
    return NNUE::networks && NNUE::networks->big.loaded() && NNUE::networks->small.loaded();
}

std::string Eval::load_default_networks(const std::string& exeDir) {
    ensure_allocated();
    std::string err;

#if defined(NEST_HAS_EMBEDDED_NETS)
    (void) exeDir;
    if (NNUE::networks->big.load(gNestBigNet, size_t(gNestBigNetEnd - gNestBigNet), err)
        && NNUE::networks->small.load(gNestSmallNet, size_t(gNestSmallNetEnd - gNestSmallNet), err))
        return "";
    return "embedded network rejected: " + err;
#else
    std::string problems;
    for (bool big : {true, false})
    {
        const std::string    name = big ? DefaultBigNet : DefaultSmallNet;
        std::vector<uint8_t> data;
        bool                 done = false;
        for (const std::string& dir : {std::string(""), exeDir})
        {
            if (!read_file(dir + name, data))
                continue;
            done = big ? NNUE::networks->big.load(data.data(), data.size(), err)
                       : NNUE::networks->small.load(data.data(), data.size(), err);
            if (done)
                break;
            problems += name + ": " + err + ". ";
        }
        if (!done && problems.find(name) == std::string::npos)
            problems += name + " not found. ";
    }
    if (!problems.empty())
        problems += "Using the fallback evaluation, which is far weaker; set EvalFile / EvalFileSmall.";
    return problems;
#endif
}

std::string Eval::load_network_file(const std::string& path, bool big) {
    ensure_allocated();
    std::vector<uint8_t> data;
    if (!read_file(path, data))
        return "cannot read network file " + path;

    std::string err;
    const bool  ok = big ? NNUE::networks->big.load(data.data(), data.size(), err)
                         : NNUE::networks->small.load(data.data(), data.size(), err);
    if (ok)
        return "";

    // A rejected file must not leave us without a network.
    std::string restore = load_default_networks("");
    return path + " rejected: " + err + (restore.empty() ? "; default network kept" : "; " + restore);
}

Value Eval::evaluate(const Position& pos, AccumulatorStack& acc, int optimism) {
    assert(!pos.checkers());

    if (!networks_ready()) [[unlikely]]
        return std::clamp(classical(pos), -VALUE_MAX_EVAL, VALUE_MAX_EVAL);

    // Lopsided positions go to the small, fast network.
    bool smallNet = std::abs(simple_eval(pos, pos.side_to_move())) > 962;
    int  psqt, positional;

    if (smallNet)
        eval_small(pos, acc, psqt, positional);
    else
        eval_big(pos, acc, psqt, positional);

    int nnue = (125 * psqt + 131 * positional) / 128;

    // The small net said it is close after all: ask the big one.
    if (smallNet && std::abs(nnue) < 236)
    {
        eval_big(pos, acc, psqt, positional);
        nnue = (125 * psqt + 131 * positional) / 128;
    }

    const int complexity = std::abs(psqt - positional);
    optimism += optimism * complexity / 468;
    nnue -= nnue * complexity / 18000;

    const int material = 535 * (pos.count(W_PAWN) + pos.count(B_PAWN)) + pos.non_pawn_material();
    int       v        = (nnue * (77777 + material) + optimism * (7777 + material)) / 77777;

    // Damp the evaluation as the 50-move counter grows.
    v -= v * pos.rule50_count() / 212;

    return std::clamp(v, -VALUE_MAX_EVAL, VALUE_MAX_EVAL);
}

int Eval::to_cp(Value v, const Position& pos) {
    if (!networks_ready())
        return v / 2;
    return int(std::round(100 * int(v) / win_rate_params(pos).a));
}

std::array<int, 3> Eval::wdl(Value v, const Position& pos) {
    const auto [a, b] = win_rate_params(pos);
    const int w       = int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
    const int l       = int(0.5 + 1000 / (1 + std::exp((a + double(v)) / b)));
    return {w, 1000 - w - l, l};
}

std::string Eval::trace(const Position& pos) {
    std::ostringstream ss;

    if (pos.checkers())
        return "Final evaluation: none (in check)\n";

    if (!networks_ready())
    {
        const Value v = classical(pos);
        ss << "No network loaded. Fallback evaluation (internal units, side to move): " << v << "\n";
        return ss.str();
    }

    auto             acc = std::make_unique<AccumulatorStack>();
    const int        sign = pos.side_to_move() == WHITE ? 1 : -1;
    const int        bucket = layer_bucket(pos);
    NNUE::Accumulator<NNUE::L1Big>*   big   = new NNUE::Accumulator<NNUE::L1Big>;
    NNUE::Accumulator<NNUE::L1Small>* small = new NNUE::Accumulator<NNUE::L1Small>;
    NNUE::refresh_from_scratch(pos, NNUE::networks->big, *big);
    NNUE::refresh_from_scratch(pos, NNUE::networks->small, *small);

    ss << "Network outputs, internal units, white point of view (* = bucket in use)\n"
       << " bucket |  big psqt  big pos | small psqt small pos\n";
    for (int b = 0; b < NNUE::LayerStacks; ++b)
    {
        int p1, q1, p2, q2;
        NNUE::networks->big.evaluate(*big, pos.side_to_move(), b, p1, q1);
        NNUE::networks->small.evaluate(*small, pos.side_to_move(), b, p2, q2);
        ss << std::setw(6) << b << (b == bucket ? " *" : "  ") << "|" << std::setw(10) << sign * p1
           << std::setw(9) << sign * q1 << " |" << std::setw(11) << sign * p2 << std::setw(10)
           << sign * q2 << "\n";
    }
    delete big;
    delete small;

    int p, q;
    eval_big(pos, *acc, p, q);
    ss << "Big net raw (psqt + positional, side to move): " << p + q << "\n";
    eval_small(pos, *acc, p, q);
    ss << "Small net raw (psqt + positional, side to move): " << p + q << "\n";

    const Value v = evaluate(pos, *acc, 0);
    ss << "Final evaluation (internal units, side to move): " << v << "\n"
       << "Final evaluation: " << std::showpos << std::fixed << std::setprecision(2)
       << 0.01 * to_cp(sign * v, pos) << " (white side)\n";
    return ss.str();
}

}  // namespace Nest
