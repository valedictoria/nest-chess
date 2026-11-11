#!/usr/bin/env python3
"""Smoke test: plays engine-vs-itself games over UCI with a real clock.

It checks the things a Lichess bot must never get wrong: the engine always
answers, never sends a move the other instance rejects, never oversteps the
clock, and handles ponder / ponderhit / stop. It does not measure strength.

    tools/selfplay_smoke.py ./nest --games 4 --time 2000 --inc 50 [--ponder] [--chess960]
"""
import argparse, subprocess, sys, time


# Chess960 starting positions with the king and rooks in varied places.
CHESS960_STARTS = [
    "nrbqkbrn/pppppppp/8/8/8/8/PPPPPPPP/NRBQKBRN w KQkq - 0 1",
    "rkbnqbnr/pppppppp/8/8/8/8/PPPPPPPP/RKBNQBNR w KQkq - 0 1",
    "bbqnnrkr/pppppppp/8/8/8/8/PPPPPPPP/BBQNNRKR w KQkq - 0 1",
    "rnkrbbqn/pppppppp/8/8/8/8/PPPPPPPP/RNKRBBQN w KQkq - 0 1",
]


class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        self.send("uci"); self.wait("uciok")
        self.send("isready"); self.wait("readyok")

    def send(self, s):
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def wait(self, token):
        """Reads until a line starting with token; returns (line, all lines)."""
        lines = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("engine exited (crash?) while waiting for " + token)
            line = line.strip(); lines.append(line)
            if line.startswith(token):
                return line, lines

    def quit(self):
        self.send("quit"); self.p.wait(timeout=10)


def play(path, base, inc, ponder, max_plies, fen=None):
    engines = [Engine(path), Engine(path)]
    root = "position startpos"
    for e in engines:
        if fen:
            e.send("setoption name UCI_Chess960 value true")
        e.send("ucinewgame")
    if fen:
        root = "position fen " + fen
    clock = [base, base]
    moves, worst_margin = [], base
    pondering = [None, None]          # move each engine is pondering on, if any
    result = "unfinished"
    for ply in range(max_plies):
        stm = ply % 2
        e = engines[stm]
        t0 = time.monotonic()
        if ponder and pondering[stm] is not None:
            if moves and pondering[stm] == moves[-1]:
                e.send("ponderhit")
            else:
                e.send("stop"); e.wait("bestmove")          # discard the ponder search
                e.send(root + " moves " + " ".join(moves))
                e.send(f"go wtime {clock[0]} btime {clock[1]} winc {inc} binc {inc}")
            pondering[stm] = None
        else:
            e.send(root + (" moves " + " ".join(moves) if moves else ""))
            e.send(f"go wtime {clock[0]} btime {clock[1]} winc {inc} binc {inc}")
        line, lines = e.wait("bestmove")
        used = int((time.monotonic() - t0) * 1000)
        if any("invalid FEN or illegal move" in l for l in lines):
            raise RuntimeError(f"engine rejected the game moves: {' '.join(moves)}")
        clock[stm] -= used
        worst_margin = min(worst_margin, clock[stm])
        if clock[stm] <= 0:
            raise RuntimeError(f"time forfeit at ply {ply}: used {used} ms")
        clock[stm] += inc
        parts = line.split()
        if parts[1] == "(none)":
            result = "mate or stalemate"
            break
        moves.append(parts[1])
        if ponder and len(parts) >= 4 and parts[2] == "ponder":
            e.send(root + " moves " + " ".join(moves + [parts[3]]))
            e.send(f"go ponder wtime {clock[0]} btime {clock[1]} winc {inc} binc {inc}")
            pondering[stm] = parts[3]
    for i, e in enumerate(engines):
        if pondering[i] is not None:
            e.send("stop"); e.wait("bestmove")
        e.quit()
    return len(moves), result, worst_margin


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("--games", type=int, default=4)
    ap.add_argument("--time", type=int, default=2000, help="base time, ms")
    ap.add_argument("--inc", type=int, default=50, help="increment, ms")
    ap.add_argument("--ponder", action="store_true")
    ap.add_argument("--max-plies", type=int, default=400)
    ap.add_argument("--chess960", action="store_true", help="play from Chess960 starting positions")
    a = ap.parse_args()
    for g in range(a.games):
        fen = CHESS960_STARTS[g % len(CHESS960_STARTS)] if a.chess960 else None
        plies, result, margin = play(a.engine, a.time, a.inc, a.ponder, a.max_plies, fen)
        print(f"game {g + 1}: {plies} plies, {result}, lowest clock {margin} ms", flush=True)
    print("OK")


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as err:
        print("FAILED:", err)
        sys.exit(1)
