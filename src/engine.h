/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <string>
#include <vector>

#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"

namespace Nest {

constexpr const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// The engine core. The UCI and XBoard frontends are thin layers over this
// class and contain no search logic.
class Engine {
   public:
    Engine();
    ~Engine();

    // Returns false and leaves the previous position in place if the FEN is
    // invalid or a move is not legal.
    bool set_position(const std::string& fen, const std::vector<std::string>& moves);
    bool push_move(const std::string& move);
    bool pop_move();

    void go(const Search::Limits& limits);
    void stop();
    void ponderhit();
    void wait_for_search_finished();
    void new_game();

    // Returns an empty string on success, otherwise what was wrong.
    std::string set_option(const std::string& name, const std::string& value);
    std::string options_uci() const;

    uint64_t perft(int depth, bool divide);

    // Loads the networks compiled into the binary, or the default files
    // next to it. Returns a message if something needs the user's attention.
    std::string load_default_networks(const std::string& argv0);
    int         hashfull() const { return tt.hashfull(); }

    const Position& position() const { return pos; }
    std::string     move_string(Move m) const;
    Move            parse_move(const std::string& str) const;
    uint64_t        nodes_searched() const { return threads.nodes_searched(); }

    void set_callbacks(Search::Callbacks cb) { threads.callbacks = std::move(cb); }

    struct Options {
        int         hash         = 16;
        int         threadCount  = 1;
        int         multiPV      = 1;
        int         moveOverhead = 10;
        bool        ponder       = false;
        bool        chess960     = false;
        bool        showWDL      = false;
    } options;

   private:
    TranspositionTable       tt;
    Position                 pos;
    StateList                states;
    std::string              rootFen;
    std::vector<std::string> gameMoves;
    ThreadPool               threads;
};

std::string move_to_string(Move m, bool chess960);

}  // namespace Nest
