/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "uci.h"

#include <iostream>
#include <mutex>
#include <sstream>

#include "eval.h"
#include "misc.h"

namespace Nest {

void println(const std::string& line) {
    static std::mutex           m;
    std::lock_guard<std::mutex> lock(m);
    std::cout << line << std::endl;
}

namespace UCI {

std::string score_string(Value v, const Position& pos) {
    if (v >= VALUE_MATE_IN_MAX_PLY)
        return "mate " + std::to_string((VALUE_MATE - v + 1) / 2);
    if (v <= VALUE_MATED_IN_MAX_PLY)
        return "mate " + std::to_string(-(VALUE_MATE + v) / 2);
    return "cp " + std::to_string(Eval::to_cp(v, pos));
}

namespace {

void install_callbacks(Engine& engine) {
    Search::Callbacks cb;
    cb.onInfo = [&engine](const Search::Info& i) {
        std::ostringstream ss;
        ss << "info depth " << i.depth << " seldepth " << i.selDepth << " multipv " << i.multiPV
           << " score " << score_string(i.score, engine.position());
        if (i.lowerbound)
            ss << " lowerbound";
        if (i.upperbound)
            ss << " upperbound";
        if (engine.options.showWDL)
        {
            const auto wdl = Eval::wdl(i.score, engine.position());
            ss << " wdl " << wdl[0] << " " << wdl[1] << " " << wdl[2];
        }
        ss << " nodes " << i.nodes << " nps " << i.nps << " hashfull " << i.hashfull
           << " tbhits 0 time " << i.time << " pv";
        for (Move m : i.pv)
            ss << " " << engine.move_string(m);
        println(ss.str());
    };
    cb.onCurrMove = [&engine](int depth, Move m, int number) {
        println("info depth " + std::to_string(depth) + " currmove " + engine.move_string(m)
                + " currmovenumber " + std::to_string(number));
    };
    cb.onBestMove = [&engine](Move best, Move ponder) {
        std::string s = "bestmove " + engine.move_string(best);
        if (ponder != Move::none())
            s += " ponder " + engine.move_string(ponder);
        println(s);
    };
    engine.set_callbacks(cb);
}

void position(Engine& engine, std::istringstream& is) {
    std::string token, fen;
    is >> token;
    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token;  // "moves", if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
    {
        println("info string position: expected startpos or fen");
        return;
    }

    std::vector<std::string> moves;
    while (is >> token)
        moves.push_back(token);

    if (!engine.set_position(fen, moves))
        println("info string position: invalid FEN or illegal move, command ignored in part");
}

void go(Engine& engine, std::istringstream& is) {
    Search::Limits limits;
    limits.startTime = now();
    std::string token;
    bool        perft = false;
    int         perftDepth = 0;

    auto number = [&](auto& target) {
        long long v = 0;
        if (is >> v)
            target = static_cast<std::remove_reference_t<decltype(target)>>(std::max(v, 0LL));
    };

    while (is >> token)
    {
        if (token == "searchmoves")
        {
            while (is >> token)
            {
                const Move m = engine.parse_move(token);
                if (m != Move::none())
                    limits.searchmoves.push_back(m);
            }
        }
        else if (token == "wtime")
            number(limits.time[WHITE]);
        else if (token == "btime")
            number(limits.time[BLACK]);
        else if (token == "winc")
            number(limits.inc[WHITE]);
        else if (token == "binc")
            number(limits.inc[BLACK]);
        else if (token == "movestogo")
            number(limits.movestogo);
        else if (token == "depth")
            number(limits.depth);
        else if (token == "nodes")
            number(limits.nodes);
        else if (token == "movetime")
            number(limits.movetime);
        else if (token == "mate")
            number(limits.mate);
        else if (token == "perft")
        {
            perft = true;
            number(perftDepth);
        }
        else if (token == "infinite")
            limits.infinite = true;
        else if (token == "ponder")
            limits.ponder = true;
    }

    // A clock that has run out still has to produce a move.
    for (Color c : {WHITE, BLACK})
        if (limits.time[c] == 0 && limits.inc[c] != 0)
            limits.time[c] = 1;

    if (perft)
    {
        const TimePoint t0 = now();
        const uint64_t  n  = engine.perft(perftDepth, true);
        const TimePoint t  = std::max<TimePoint>(1, now() - t0);
        println("\nNodes searched: " + std::to_string(n) + "\nTime (ms): " + std::to_string(t)
                + "\nNodes/second: " + std::to_string(n * 1000 / uint64_t(t)));
        return;
    }
    engine.go(limits);
}

void setoption(Engine& engine, std::istringstream& is) {
    std::string token, name, value;
    is >> token;  // "name"
    while (is >> token && token != "value")
        name += (name.empty() ? "" : " ") + token;
    while (is >> token)
        value += (value.empty() ? "" : " ") + token;

    const std::string err = engine.set_option(name, value);
    if (!err.empty())
        println("info string " + err);
}

}  // namespace

bool execute(Engine& engine, const std::string& line) {
    std::istringstream is(line);
    std::string        token;
    is >> std::skipws >> token;

    if (token == "quit")
    {
        engine.stop();
        return false;
    }
    else if (token == "stop")
        engine.stop();
    else if (token == "ponderhit")
        engine.ponderhit();
    else if (token == "uci")
        println("id name " + engine_info() + "\nid author the Nest developers\n\n"
                + engine.options_uci() + "\nuciok");
    else if (token == "isready")
        println("readyok");
    else if (token == "ucinewgame")
        engine.new_game();
    else if (token == "setoption")
        setoption(engine, is);
    else if (token == "position")
        position(engine, is);
    else if (token == "go")
        go(engine, is);
    else if (token == "bench")
    {
        std::string args;
        std::getline(is, args);
        Bench::run(engine, args);
        install_callbacks(engine);
    }
    else if (token == "speedtest")
    {
        std::string args;
        std::getline(is, args);
        Bench::speedtest(engine, args);
        install_callbacks(engine);
    }
    else if (token == "perft")
    {
        int depth = 1;
        is >> depth;
        std::istringstream sub("perft " + std::to_string(depth));
        go(engine, sub);
    }
    else if (token == "d")
    {
        engine.wait_for_search_finished();
        println(engine.position().pretty() + Eval::trace(engine.position()));
    }
    else if (token == "eval")
    {
        engine.wait_for_search_finished();
        println(Eval::trace(engine.position()));
    }
    else if (token == "compiler")
        println(compiler_info());
    else if (!token.empty() && token[0] != '#')
        println("Unknown command: '" + line + "'. Type help for more information.");

    return true;
}

void run_once(Engine& engine, const std::string& line) {
    install_callbacks(engine);
    execute(engine, line);
    engine.wait_for_search_finished();
}

void loop(Engine& engine, const std::string& firstLine) {
    install_callbacks(engine);

    std::string line = firstLine;
    if (!line.empty() && !execute(engine, line))
        return;

    while (std::getline(std::cin, line))
        if (!execute(engine, line))
            return;

    // End of input: behave like "quit", but let a running search finish its
    // output first (useful when commands are piped in).
    engine.wait_for_search_finished();
}

}  // namespace UCI
}  // namespace Nest
