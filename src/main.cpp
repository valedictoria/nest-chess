/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include <iostream>
#include <string>

#include "bitboard.h"
#include "engine.h"
#include "eval.h"
#include "misc.h"
#include "position.h"
#include "uci.h"
#include "xboard.h"

using namespace Nest;

int main(int argc, char* argv[]) {
    Bitboards::init();
    Position::init();

    Engine            engine;
    const std::string netMessage = engine.load_default_networks(argc > 0 ? argv[0] : "");
    if (!netMessage.empty())
        println("info string " + netMessage);

    // Arguments form one command that is run before exiting,
    // e.g. "./nest bench" (which is how OpenBench calls an engine).
    if (argc > 1)
    {
        std::string cmd;
        for (int i = 1; i < argc; ++i)
            cmd += std::string(i > 1 ? " " : "") + argv[i];
        UCI::run_once(engine, cmd);
        return 0;
    }

    std::string first;
    if (!std::getline(std::cin, first))
        return 0;
    if (first == "xboard")
        XBoard::loop(engine);
    else
        UCI::loop(engine, first);
    return 0;
}
