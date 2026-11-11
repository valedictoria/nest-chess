/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "misc.h"
#include "uci.h"

namespace Nest::Bench {

namespace {

// A fixed mix of openings, middlegames and endgames. The node count of
// "bench" over these positions is the engine's signature: it must only
// change when search or evaluation behaviour changes.
const std::vector<std::string> Positions = {
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
  "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
  "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 11",
  "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
  "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
  "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
  "1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B2/2K5 b - - 0 1",
  "3r1k2/4npp1/1ppr3p/p6P/P2PPPP1/1NR5/5K2/2R5 w - - 0 1",
  "2q1rr1k/3bbnnp/p2p1pp1/2pPp3/PpP1P1P1/1P2BNNP/2BQ1PRK/7R b - - 0 1",
  "rnbqkb1r/p3pppp/1p6/2ppP3/3N4/2P5/PPP1QPPP/R1B1KB1R w KQkq - 0 1",
  "r1b2rk1/2q1b1pp/p2ppn2/1p6/3QP3/1BN1B3/PPP3PP/R4RK1 w - - 0 1",
  "2r3k1/pppR1pp1/4p3/4P1P1/5P2/1P4K1/P1P5/8 w - - 0 1",
  "1nk1r1r1/pp2n1pp/4p3/q2pPp1N/b1pP1P2/B1P2R2/2P1B1PP/R2Q2K1 w - - 0 1",
  "4b3/p3kp2/6p1/3pP2p/2pP1P2/4K1P1/P3N2P/8 w - - 0 1",
  "2kr1bnr/pbpq4/2n1pp2/3p3p/3P1P1B/2N2N1Q/PPP3PP/2KR1B1R w - - 0 1",
  "3rr1k1/pp3pp1/1qn2np1/8/3p4/PP1R1P2/2P1NQPP/R1B3K1 b - - 0 1",
  "2r1nrk1/p2q1ppp/bp1p4/n1pPp3/P1P1P3/2PBB1N1/4QPPP/R4RK1 w - - 0 1",
  "r3r1k1/ppqb1ppp/8/4p1NQ/8/2P5/PP3PPP/R3R1K1 b - - 0 1",
  "r2q1rk1/4bppp/p2p4/2pP4/3pP3/3Q4/PP1B1PPP/R3R1K1 w - - 0 1",
  "rnb2r1k/pp2p2p/2pp2p1/q2P1p2/8/1Pb2NP1/PB2PPBP/R2Q1RK1 w - - 0 1",
  "2r3k1/1p2q1pp/2b1pr2/p1pp4/6Q1/1P1PP1R1/P1PN2PP/5RK1 w - - 0 1",
  "r1bqkb1r/4npp1/p1p4p/1p1pP1B1/8/1B6/PPPN1PPP/R2Q1RK1 w kq - 0 1",
  "r2q1rk1/1ppnbppp/p2p1nb1/3Pp3/2P1P1P1/2N2N1P/PPB1QP2/R1B2RK1 b - - 0 1",
  "r1bq1rk1/pp2ppbp/2np2p1/2n5/P3PP2/N1P2N2/1PB3PP/R1B1QRK1 b - - 0 1",
  "3rr3/2pq2pk/p2p1pnp/8/2QBPP2/1P6/P5PP/4RRK1 b - - 0 1",
  "r4k2/pb2bp1r/1p1qp2p/3pNp2/3P1P2/2N3P1/PPP1Q2P/2KRR3 w - - 0 1",
  "3rn2k/ppb2rpp/2ppqp2/5N2/2P1P3/1P5Q/PB3PPP/3RR1K1 w - - 0 1",
  "2r2rk1/1bqnbpp1/1p1ppn1p/pP6/N1P1P3/P2B1N1P/1B2QPP1/R2R2K1 b - - 0 1",
  "r1bqk2r/pp2bppp/2p5/3pP3/P2Q1P2/2N1B3/1PP3PP/R4RK1 b kq - 0 1",
  "r2qnrnk/p2b2b1/1p1p2pp/2pPpp2/1PP1P3/PRNBB3/3QNPPP/5RK1 w - - 0 1",
  "8/8/8/8/5kp1/P7/8/1K1N4 w - - 0 1",
  "8/3k4/8/8/8/4B3/4KB2/2B5 w - - 0 1",
  "8/2p4P/8/kr6/6R1/8/8/1K6 w - - 0 1",
  "6k1/6p1/6Pp/ppp5/3pn2P/1P3K2/1PP2P2/3N4 b - - 0 1",
  "5rk1/q6p/2p3bR/1pPp1rP1/1P1Pp3/P3B1Q1/1K3P2/R7 w - - 93 90",
  "4rrk1/1p1nq3/p7/2p1P1pp/3P2bp/3Q1Bn1/PPPB4/1K2R1NR w - - 40 21",
  "r3k2r/3nnpbp/q2pp1p1/p7/Pp1PPPP1/4BNN1/1P5P/R2Q1RK1 w kq - 0 16",
  "3q2k1/pb3p1p/4pbp1/2r5/PpN2N2/1P2P2P/5PP1/Q2R2K1 b - - 4 26",
  "2rqkb1r/ppp2p2/2npb1p1/1N1Nn2p/2P1PP2/8/PP2B1PP/R1BQK2R b KQ - 0 11",
  "r1bbk1nr/pp3p1p/2n5/1N4p1/2Np1B2/8/PPP2PPP/2KR1B1R w kq - 0 13",
  "r1bq1rk1/ppp1nppp/4n3/3p3Q/3P4/1BP1B3/PP1N2PP/R4RK1 w - - 1 16",
  "8/6pk/1p6/8/PP3p1p/5P2/4KP1q/3Q4 w - - 0 1",
  "7k/3p2pp/4q3/8/4Q3/5Kp1/P6b/8 w - - 0 1",
  "8/8/8/5N2/8/p7/8/2NK3k w - - 0 1",
  "8/2k1p3/3pP3/3P2K1/8/8/8/8 w - - 0 1",
  "8/R7/2q5/8/6k1/8/1P5p/K6R w - - 0 124",
};

void silence(Engine& engine) {
    Search::Callbacks cb;
    cb.onInfo     = [](const Search::Info&) {};
    cb.onBestMove = [](Move, Move) {};
    engine.set_callbacks(cb);
}

}  // namespace

void run(Engine& engine, const std::string& args) {
    std::istringstream is(args);
    int                hash = 16, threadCount = 1, depth = 13;
    std::string        fenFile;
    is >> hash >> threadCount >> depth >> fenFile;

    std::vector<std::string> fens = Positions;
    if (!fenFile.empty() && fenFile != "default")
    {
        std::ifstream f(fenFile);
        if (!f)
        {
            println("info string bench: cannot open " + fenFile);
            return;
        }
        fens.clear();
        for (std::string line; std::getline(f, line);)
            if (!line.empty() && line[0] != '#')
                fens.push_back(line);
    }

    // Fixed settings make the node count reproducible; they are restored.
    const auto saved = engine.options;
    engine.set_option("Hash", std::to_string(hash));
    engine.set_option("Threads", std::to_string(threadCount));
    engine.set_option("MultiPV", "1");
    engine.set_option("UCI_Chess960", "false");
    engine.new_game();
    silence(engine);

    uint64_t        nodes = 0;
    int             n     = 0;
    const TimePoint start = now();

    for (const std::string& fen : fens)
    {
        ++n;
        if (!engine.set_position(fen, {}))
        {
            println("info string bench: invalid FEN skipped: " + fen);
            continue;
        }
        Search::Limits limits;
        limits.startTime = now();
        limits.depth     = depth;
        engine.go(limits);
        engine.wait_for_search_finished();
        nodes += engine.nodes_searched();
        println("Position " + std::to_string(n) + "/" + std::to_string(fens.size()) + ": "
                + std::to_string(engine.nodes_searched()) + " nodes");
    }

    const TimePoint elapsed = std::max<TimePoint>(1, now() - start);
    const uint64_t  nps     = nodes * 1000 / uint64_t(elapsed);

    println("\n===========================\nTotal time (ms) : " + std::to_string(elapsed)
            + "\nNodes searched  : " + std::to_string(nodes) + "\nNodes/second    : "
            + std::to_string(nps) + "\n" + std::to_string(nodes) + " nodes " + std::to_string(nps)
            + " nps");

    engine.set_option("Hash", std::to_string(saved.hash));
    engine.set_option("Threads", std::to_string(saved.threadCount));
    engine.set_option("MultiPV", std::to_string(saved.multiPV));
    engine.set_option("UCI_Chess960", saved.chess960 ? "true" : "false");
    engine.set_position(StartFEN, {});
}

void speedtest(Engine& engine, const std::string& args) {
    std::istringstream is(args);
    int maxThreads = int(std::max(1u, std::thread::hardware_concurrency())), hash = 256, seconds = 5;
    is >> maxThreads >> hash >> seconds;
    maxThreads = std::max(maxThreads, 1);
    seconds    = std::max(seconds, 1);

    const auto saved = engine.options;
    engine.set_option("Hash", std::to_string(hash));
    silence(engine);

    // A few positions, the same wall time each, at 1, 2, 4, ... threads.
    const std::vector<std::string> fens = {Positions[1], Positions[5], Positions[12], Positions[22]};
    uint64_t                       baseNps = 0;

    println("threads        nps   scaling");
    for (int t = 1;; t = std::min(t * 2, maxThreads))
    {
        engine.set_option("Threads", std::to_string(t));
        uint64_t  nodes   = 0;
        TimePoint elapsed = 0;
        for (const std::string& fen : fens)
        {
            engine.new_game();
            engine.set_position(fen, {});
            Search::Limits limits;
            limits.startTime = now();
            limits.movetime  = TimePoint(seconds) * 1000 / TimePoint(fens.size());
            engine.go(limits);
            engine.wait_for_search_finished();
            nodes += engine.nodes_searched();
            elapsed += now() - limits.startTime;
        }
        const uint64_t nps = nodes * 1000 / uint64_t(std::max<TimePoint>(1, elapsed));
        if (t == 1)
            baseNps = std::max<uint64_t>(1, nps);

        std::ostringstream ss;
        ss.precision(2);
        ss << std::fixed;
        ss.width(7);
        ss << t;
        ss.width(11);
        ss << nps;
        ss.width(9);
        ss << double(nps) / double(baseNps) << "x  (" << double(nps) / double(baseNps) / t * 100 << "% of linear)";
        println(ss.str());
        if (t >= maxThreads)
            break;
    }

    engine.set_option("Hash", std::to_string(saved.hash));
    engine.set_option("Threads", std::to_string(saved.threadCount));
    engine.set_position(StartFEN, {});
}

}  // namespace Nest::Bench
