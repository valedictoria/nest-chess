/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <string>

#include "engine.h"

namespace Nest {

// Thread-safe line output shared by all frontends.
void println(const std::string& line);

namespace UCI {
// Runs the UCI loop on stdin, starting with `firstLine` if it is not empty.
// Returns when "quit" or end of input is seen.
void loop(Engine& engine, const std::string& firstLine);
// Runs a single command (from the command line) and waits for it to finish.
void run_once(Engine& engine, const std::string& line);
// Executes one command line; returns false on "quit".
bool execute(Engine& engine, const std::string& line);
std::string score_string(Value v, const Position& pos);
}

namespace Bench {
// bench [hash] [threads] [depth] [fenfile]
void run(Engine& engine, const std::string& args);
// speedtest [threads] [hash] [seconds]: nodes per second at 1, 2, 4, ... threads
void speedtest(Engine& engine, const std::string& args);
}

}  // namespace Nest
