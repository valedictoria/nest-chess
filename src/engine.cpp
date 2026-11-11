/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "engine.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include <cstdlib>

#include "eval.h"
#include "movegen.h"
#include "tune.h"

namespace Nest {

std::string move_to_string(Move m, bool chess960) {
    if (m == Move::none())
        return "(none)";
    if (m == Move::null())
        return "0000";

    const Square from = m.from_sq();
    Square       to   = m.to_sq();

    // Castling is stored as king-takes-rook; standard chess prints e1g1.
    if (m.type_of() == CASTLING && !chess960)
        to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

    std::string s{char('a' + file_of(from)), char('1' + rank_of(from)), char('a' + file_of(to)),
                  char('1' + rank_of(to))};
    if (m.type_of() == PROMOTION)
        s += " pnbrqk"[m.promotion_type()];
    return s;
}

Engine::Engine() {
    tt.resize(size_t(options.hash), size_t(options.threadCount));
    threads.set(size_t(options.threadCount), tt);
    set_position(StartFEN, {});
}

std::string Engine::load_default_networks(const std::string& argv0) {
    const size_t slash = argv0.find_last_of("/\\");
    return Eval::load_default_networks(slash == std::string::npos ? "" : argv0.substr(0, slash + 1));
}

Engine::~Engine() {
    stop();
    wait_for_search_finished();
    threads.release();
}

std::string Engine::move_string(Move m) const { return move_to_string(m, pos.is_chess960()); }

Move Engine::parse_move(const std::string& str) const {
    std::string s = str;
    if (s.size() == 5)
        s[4] = char(std::tolower(s[4]));
    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (s == move_string(m))
            return m;
        // Accept king-takes-rook castling in standard chess as well.
        if (m.move.type_of() == CASTLING && s == move_to_string(m, true))
            return m;
    }
    return Move::none();
}

bool Engine::set_position(const std::string& fen, const std::vector<std::string>& moves) {
    wait_for_search_finished();

    // Validate on scratch objects first, so that a bad command changes nothing.
    {
        StateInfo st;
        Position  probe;
        if (!probe.set(fen, options.chess960, &st))
            return false;
    }

    states.clear();
    states.emplace_back();
    pos.set(fen, options.chess960, &states.back());
    rootFen = fen;
    gameMoves.clear();

    for (const auto& ms : moves)
        if (!push_move(ms))
            return false;
    return true;
}

bool Engine::push_move(const std::string& move) {
    wait_for_search_finished();
    const Move m = parse_move(move);
    if (m == Move::none())
        return false;
    states.emplace_back();
    pos.do_move(m, states.back());
    gameMoves.push_back(move);
    return true;
}

bool Engine::pop_move() {
    if (gameMoves.empty())
        return false;
    auto moves = gameMoves;
    moves.pop_back();
    return set_position(std::string(rootFen), moves);
}

void Engine::go(const Search::Limits& limits) {
    threads.settings.multiPV      = options.multiPV;
    threads.settings.moveOverhead = options.moveOverhead;
    threads.settings.ponder       = options.ponder;
    threads.start_thinking(pos, states, limits);
}

void Engine::stop() { threads.stop = true; }

void Engine::ponderhit() { threads.ponder = false; }

void Engine::wait_for_search_finished() { threads.wait_for_search_finished(); }

void Engine::new_game() {
    wait_for_search_finished();
    tt.clear(size_t(options.threadCount));
    threads.clear();
}

namespace {

uint64_t perft_rec(Position& pos, int depth) {
    MoveList<LEGAL> moves(pos);
    if (depth <= 1)
        return moves.size();
    uint64_t  n = 0;
    StateInfo st;
    for (const auto& m : moves)
    {
        pos.do_move(m, st);
        n += perft_rec(pos, depth - 1);
        pos.undo_move(m);
    }
    return n;
}

bool parse_int(const std::string& s, int min, int max, int& out) {
    if (s.empty())
        return false;
    char*           end = nullptr;
    const long long v   = std::strtoll(s.c_str(), &end, 10);
    if (*end != '\0' || v < min || v > max)
        return false;
    out = int(v);
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

}  // namespace

uint64_t Engine::perft(int depth, bool divide) {
    wait_for_search_finished();
    if (depth < 1)
        return 1;
    uint64_t  total = 0;
    StateInfo st;
    for (const auto& m : MoveList<LEGAL>(pos))
    {
        uint64_t n = 1;
        if (depth > 1)
        {
            pos.do_move(m, st);
            n = perft_rec(pos, depth - 1);
            pos.undo_move(m);
        }
        total += n;
        if (divide)
            std::cout << move_string(m) << ": " << n << std::endl;
    }
    return total;
}

std::string Engine::options_uci() const {
    std::ostringstream ss;
    ss << "option name Hash type spin default 16 min 1 max 1048576\n"
       << "option name Threads type spin default 1 min 1 max 1024\n"
       << "option name MultiPV type spin default 1 min 1 max 256\n"
       << "option name Ponder type check default false\n"
       << "option name Move Overhead type spin default 10 min 0 max 5000\n"
       << "option name EvalFile type string default " << Eval::DefaultBigNet << "\n"
       << "option name EvalFileSmall type string default " << Eval::DefaultSmallNet << "\n"
       << "option name UCI_Chess960 type check default false\n"
       << "option name UCI_ShowWDL type check default false\n"
       << "option name Clear Hash type button";
#if defined(TUNE)
    for (const auto& p : tune_registry())
        ss << "\noption name " << p.name << " type spin default " << p.def << " min " << p.min
           << " max " << p.max;
#endif
    return ss.str();
}

std::string Engine::set_option(const std::string& name, const std::string& value) {
    wait_for_search_finished();
    const std::string n = lower(name);

    auto boolean = [&](bool& target) -> std::string {
        const std::string v = lower(value);
        if (v != "true" && v != "false")
            return "expected true or false for " + name;
        target = v == "true";
        return "";
    };
    auto spin = [&](int& target, int min, int max) -> std::string {
        return parse_int(value, min, max, target)
               ? ""
               : "value for " + name + " must be an integer in [" + std::to_string(min) + ", "
                   + std::to_string(max) + "]";
    };

    if (n == "hash")
    {
        const int         previous = options.hash;
        const std::string err      = spin(options.hash, 1, 1048576);
        if (err.empty() && !tt.resize(size_t(options.hash), size_t(options.threadCount)))
        {
            options.hash = previous;
            tt.resize(size_t(previous), size_t(options.threadCount));
            return "not enough memory for Hash " + value + " MB; keeping " + std::to_string(previous);
        }
        return err;
    }
    if (n == "threads")
    {
        const std::string err = spin(options.threadCount, 1, 1024);
        if (err.empty())
            threads.set(size_t(options.threadCount), tt);
        return err;
    }
    if (n == "multipv")
        return spin(options.multiPV, 1, 256);
    if (n == "move overhead")
        return spin(options.moveOverhead, 0, 5000);
    if (n == "ponder")
        return boolean(options.ponder);
    if (n == "uci_showwdl")
        return boolean(options.showWDL);
    if (n == "uci_chess960")
        return boolean(options.chess960);
    if (n == "evalfile" || n == "evalfilesmall")
    {
        if (value.empty() || value == "<empty>")
            return "";
        return Eval::load_network_file(value, n == "evalfile");
    }
    if (n == "clear hash")
    {
        new_game();
        return "";
    }
#if defined(TUNE)
    for (auto& p : tune_registry())
        if (n == lower(p.name))
            return parse_int(value, p.min, p.max, *p.value) ? "" : "bad value for " + name;
#endif
    return "no such option: " + name;
}

}  // namespace Nest
