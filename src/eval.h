/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <array>
#include <string>

#include "nnue/nnue.h"
#include "types.h"

namespace Nest {

class Position;

// Per-thread incremental evaluation state. Search pushes one entry per move
// made and pops it when the move is taken back.
using AccumulatorStack = NNUE::AccumulatorStack;

namespace Eval {

constexpr const char* DefaultBigNet   = "nn-1c0000000000.nnue";
constexpr const char* DefaultSmallNet = "nn-37f18f62d772.nnue";

// Static evaluation from the side to move's point of view. This is the only
// entry point search uses; it does not know what is behind it. `optimism`
// is a small search-supplied bias for the side to move. Must not be called
// when the side to move is in check.
Value evaluate(const Position& pos, AccumulatorStack& acc, int optimism = 0);

// Loads the default networks: the ones embedded in the binary, otherwise
// files with the default names in the working directory or next to the
// executable. Returns a message for the user (empty if all is well).
std::string load_default_networks(const std::string& exeDir);
// Loads a network from a file; `big` selects which of the two it replaces.
// On failure the previous network stays in use and the reason is returned.
std::string load_network_file(const std::string& path, bool big);
bool        networks_ready();

// Display conversions: centipawns where 100 means a 50% win probability,
// and win/draw/loss per mille.
int                to_cp(Value v, const Position& pos);
std::array<int, 3> wdl(Value v, const Position& pos);

std::string trace(const Position& pos);

}  // namespace Eval
}  // namespace Nest
