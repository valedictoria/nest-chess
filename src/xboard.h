/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include "engine.h"

namespace Nest::XBoard {

// Chess Engine Communication Protocol (version 2) frontend. Called after
// the "xboard" command has been read; returns on "quit" or end of input.
void loop(Engine& engine);

}  // namespace Nest::XBoard
