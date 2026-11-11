# ENGINE_SPEC.md — High-NPS Alpha-Beta NNUE Chess Engine

> **Audience:** an autonomous AI coding agent (Fable 5.2).
> **Read this whole file before writing any code.** Sections marked **MUST** are hard requirements. Sections marked **SHOULD** are strong defaults; deviate only with a measured justification. Never invent Elo claims: every strength claim needs an SPRT result (§10).

---

## 0. Mission

Build a UCI + XBoard chess engine in modern C++ that:

1. Has the **highest nodes per second we can get without losing Elo**: a fast move generator, an incremental NNUE, a lean search loop and lock-free shared memory.
2. Uses **alpha-beta with PVS** and the standard set of modern pruning, reduction and extension techniques.
3. Scales over threads with **Lazy SMP on a shared transposition table**.
4. Uses **Stockfish NNUE nets**, since we cannot generate our own training data yet. Phase 1 uses HalfKP; phase 2 matches a pinned modern Stockfish architecture.
5. Can be tested with **SPRT on a Fishtest-style framework** (self-hosted Fishtest or OpenBench, see §11).

**Golden rule:** NPS is a tool, not the goal. A change that raises NPS but loses Elo under SPRT is rejected. A change that lowers NPS but gains Elo is accepted.

---

## 1. Constraints the agent MUST respect

### 1.1 Licensing (Stockfish nets)
- Stockfish and its `.nnue` nets are **GPL-3.0**. If we embed or distribute an SF net, **this engine MUST be GPL-3.0**: ship a `LICENSE` file, credit the source of the net, and make the source available with every binary.
- Put a `NOTICE`/`README` section that names the net file and its origin (Stockfish project, net hash).
- Rating lists (CCRL, CEGT) and some tournaments treat engines that use another engine's net as **derivatives**: they may not list them, or they list them with a flag. Lichess BOT play is fine. Document this; don't hide it.

### 1.2 "HalfKP or Stockfish NNUE by default". Be precise about this:
- **HalfKP** (`HalfKP[41024] → 256x2 → 32 → 32 → 1`) is the **original 2020 NNUE** format (Stockfish 12–13 era). It is simple, well documented, and old SF HalfKP nets still load. It is **phase 1**.
- **Current Stockfish nets are NOT HalfKP.** Modern SF uses **HalfKAv2_hm** features (king buckets, horizontal mirroring), a much wider feature transformer, PSQT outputs, **layer stacks selected by piece count**, and **two nets** (big + small, picked by material imbalance). The exact layout changes between releases.
- **MUST:** for phase 2, **pin one specific Stockfish release** (tag + net filename + SHA-256). Read the architecture from that release's own source (`src/nnue/nnue_architecture.h`, `nnue_feature_transformer.h`, `features/half_ka_v2_hm.*`, `network.cpp`), and reproduce that net format **bit-exactly**. Do not guess layer sizes from memory. Verify by matching SF's static eval on a test set of FENs (§9.3).
- Keep the evaluation behind an interface so that a net trained later by us (e.g. with `bullet`) can replace the SF net without touching search.

### 1.3 Honesty rules for the agent
- Never say "+N Elo" without a finished SPRT (or a fixed-games test with error bars).
- Never report NPS from one run. Use the protocol in §9.4.
- If something is not feasible (e.g. "Fishtest out of the box for a non-Stockfish engine"), say so and propose the closest real option.

---

## 2. Tech stack

| Item | Requirement |
|---|---|
| Language | **C++20** (C++23 allowed if both GCC ≥ 13 and Clang ≥ 17 accept it) |
| Compilers | GCC and Clang, both must build warning-free with `-Wall -Wextra -Wpedantic` |
| Build | **Makefile is the primary build** (needed by Fishtest/OpenBench workers, §11). CMake optional. |
| Targets | `x86-64` (baseline), `x86-64-sse41-popcnt`, `x86-64-avx2`, `x86-64-bmi2` (PEXT), `x86-64-avx512`/`vnni`, `armv8` / `apple-silicon` (NEON) |
| Default flags | `-O3 -flto -fno-exceptions -fno-rtti -DNDEBUG`, `-march` per target |
| PGO | `make profile-build ARCH=...` runs `bench` as the training workload, then rebuilds. **MUST** be supported. |
| Net embedding | Embed the default net into the binary (`incbin` or `#embed` when available). Fall back to `EvalFile`. |
| Third-party code | Only GPL-3-compatible. Fathom (Syzygy probing) is optional in phase 3. |

### 2.1 Primary platforms (both are first-class; every milestone must build, pass tests and be benchmarked on both)

| Platform | CPU | ARCH target | What it has | What it lacks |
|---|---|---|---|---|
| **Intel Mac** (MacBook Pro 10,2, 2012) | Ivy Bridge | `x86-64-sse41-popcnt` | SSSE3, SSE4.1, POPCNT, 128-bit integer SIMD | **No AVX2, BMI1/BMI2 (no PEXT, no TZCNT), no VNNI.** AVX1 is float-only for 256-bit, so it gives nothing for NNUE integer math |
| **Apple Silicon** (M-series) | ARMv8.4+ | `apple-silicon` / `armv8-dotprod` | NEON 128-bit, **dot-product** (`vdotq_s32`), large caches | AVX-anything; mixed P-cores and E-cores |

Rules that follow from this:
- **Sliding attacks:** fancy magic bitboards on both. The PEXT path is compiled only for `bmi2` targets and is never the default on these machines.
- **Bit scans:** use `std::countr_zero` / `std::popcount` (C++20 `<bit>`). On Ivy Bridge they compile to `BSF` and `POPCNT`; don't hand-write `TZCNT`.
- **NNUE kernels needed on day one:** SSSE3/SSE4.1 (`_mm_maddubs_epi16`, `_mm_madd_epi16`) and NEON with dot-product. AVX2/AVX-512 kernels come later and are only for other machines/workers. All kernels must give identical results to the scalar reference.
- **Ivy Bridge is the slow floor.** Expect clearly lower NPS than the M-series chip. Never compare NPS across the two machines; compare A/B builds **on the same machine** (§9.4).
- **Apple Silicon threads:** report NPS scaling for P-cores only and for P+E cores. Default `Threads` recommendation in the README = number of P-cores.
- **Builds on macOS:** the Makefile must work with Apple Clang (Xcode CLT) as well as Homebrew GCC/LLVM. On Apple Silicon use `-mcpu=apple-m1` (or newer) rather than `-march=native`. On the Intel Mac use `-march=ivybridge` for local builds and the generic `sse41-popcnt` target for distribution.
- **If the Intel Mac runs Linux** (dual boot), it must also build there with GCC; huge pages via `madvise` apply only on Linux. macOS has no transparent huge pages, so allocate with normal alignment there.
- **SPRT and long tests run on the Apple Silicon machine** (faster, more games per hour). The Intel Mac is for correctness, portability and a second NPS profile.

Makefile **MUST** accept: `make -j build ARCH=<arch> COMP=<gcc|clang> EXE=<name>` and `make -j profile-build ...`. `EXE=` and `CXX=` overrides are required by OpenBench.

---

## 3. Repository layout

```
/src
  types.h          // Square, Piece, Move (16-bit), Value, Depth, Bound
  bitboard.{h,cpp} // attacks, magics / PEXT, line/between tables
  position.{h,cpp} // board state, do/undo move, zobrist, SEE, legality
  movegen.{h,cpp}  // pseudo-legal + legal generation, staged by type
  movepick.{h,cpp} // staged move picker + history tables
  nnue/            // feature sets, accumulator, layers, SIMD kernels
  eval.{h,cpp}     // eval interface: NNUE + optional small-net/HCE fallback
  search.{h,cpp}   // iterative deepening, PVS, qsearch, pruning
  tt.{h,cpp}       // shared transposition table
  thread.{h,cpp}   // thread pool, Lazy SMP, best-thread selection
  timeman.{h,cpp}  // time management
  uci.{h,cpp}      // UCI frontend
  xboard.{h,cpp}   // CECP v2 frontend
  bench.{h,cpp}    // bench / perft / speedtest commands
  misc.{h,cpp}     // prng, prefetch, large pages, timing, logging
/tests             // perft suite, eval-match FENs, SEE tests, protocol scripts
/tools             // sprt helpers, nps harness scripts
Makefile
LICENSE  NOTICE  README.md
```

---

## 4. Board representation and move generation (the biggest NPS lever outside NNUE)

**MUST:**
- **Bitboards** (one per piece type + per colour + occupancy). Also a `Piece board[64]` mailbox for O(1) lookup.
- Sliding attacks with **PEXT** when `ARCH` has BMI2 and the CPU has fast PEXT (not AMD Zen 1/2). Otherwise **fancy magic bitboards**. Choose at compile time, not at runtime.
- **16-bit move encoding** (from 6, to 6, flags 4: promotion piece / castling / en passant).
- **Legal move generation** using check masks and pin masks (no make-then-test-for-check in the generator).
- Generate by **stage**: captures / quiet / evasions. Never build all moves when a TT move or capture causes a cutoff.
- **Incremental Zobrist** keys: main key, pawn key, non-pawn keys per colour (for correction history, §6.4), material key.
- `do_move` / `undo_move` on a `StateInfo` stack (SF style), storing only what cannot be recomputed cheaply. Benchmark copy-make vs make/unmake and keep the faster one; document the numbers.
- `gives_check(move)` without making the move. `pseudo_legal(move)` to validate TT/killer moves.
- **SEE** with a threshold API: `bool see_ge(Move m, int threshold)`.
- **Chess960** support (castling as king-takes-rook internally) behind `UCI_Chess960`.

**Acceptance (MUST pass before any search work):**
- The perft suite in `/tests/perft.epd` (standard positions + Chess960 positions) matches to the last node.
- `perft 7` from startpos = 3,195,901,860.
- Report bulk-counting perft speed (Mnps) on the reference machine as the move-generation baseline.

---

## 5. NNUE evaluation

### 5.1 Phase 1: HalfKP (fast path to a working engine)
- Feature: `(king_sq, piece_sq, piece_type_colour)` from each side's view → 41,024 inputs (64 × 641).
- Net: `FeatureTransformer 41024 → 256` (per perspective, int16), concat → 512, `ClippedReLU` → int8, `512→32`, `32→32`, `32→1` (int8 weights, int32 accumulate).
- Load a compatible legacy SF HalfKP `.nnue`. Validate against the reference implementation's eval on §9.3 FENs.

### 5.2 Phase 2: modern Stockfish architecture (pinned release)
- Implement exactly what the pinned release does: HalfKAv2_hm features, PSQT accumulation, layer stacks / output buckets, big + small net selection, any special activations (e.g. pairwise multiply, squared CReLU) **as found in that source**.
- Hash-check the net file on load; refuse to load an incompatible net, printing a clear `info string` error.

### 5.3 Performance requirements (both phases)
- **Incremental accumulator updates**: on `do_move`, record dirty pieces (≤ 3 changed features). Update **lazily** (only when eval is actually called), walking back up the stack to the last computed accumulator.
- **King moves:** for king-bucketed features, refresh with an **accumulator refresh cache ("Finny tables")**: cache per (king bucket, perspective) the last accumulator + piece bitboards, and apply only the difference.
- **SIMD:** AVX-512 / AVX2 / SSSE3 / NEON kernels for: feature-transformer add/sub (int16), CReLU + pack to int8, affine layers (`maddubs`/`VNNI dpbusd`). Scalar fallback must give **identical** results.
- Align weights and accumulators to 64 bytes. Keep the active accumulator data in L1-friendly layout. Use "update then sub" fused kernels (add N, sub M in one pass).
- Weight permutation at load time so that pack instructions need no shuffle in the hot path.
- **No eval cache** unless it is measured to help. The TT stores static eval already.

### 5.4 Interface
```cpp
Value evaluate(const Position& pos, AccumulatorStack& acc); // side-to-move POV
```
Search never knows which net or architecture is behind it.

---

## 6. Search

### 6.1 Core
- **Iterative deepening** with **aspiration windows** (start ±~15cp around the previous score, widen geometrically on fail, reduce depth slightly after fail-high as SF does).
- **PVS / NegaScout:** full window for the first move, null-window for the rest, re-search on `alpha < score < beta`.
- Node types as template parameters (`PV`, `NonPV`, `Root`) so that the compiler removes dead branches.
- **Quiescence search:** captures + queen promotions; evasions when in check. Stand-pat, delta pruning, SEE pruning of losing captures, TT probe.
- **Mate distance pruning**, repetition detection (using the key history, including "upcoming repetition" / cuckoo tables as a later SPRT item), 50-move rule, insufficient material.

### 6.2 Pruning, reductions and extensions (implement in this order; each one gets its own SPRT)
1. Transposition-table cutoffs (non-PV only, with depth and bound checks).
2. Reverse futility pruning (static null move).
3. Null-move pruning with verification at high depth; adaptive R by depth and eval margin. Disable in zugzwang-prone positions (no non-pawn material).
4. Razoring.
5. Internal iterative reduction (IIR) when there is no TT move.
6. ProbCut.
7. Late move reductions using a log-log table `R = a + ln(depth)·ln(moveCount)/b`, adjusted by: PV node, improving, history score, TT-move capture, cut-node, gives check.
8. Late move pruning (move-count based) and futility pruning for quiets.
9. SEE pruning for quiets and captures at low depth.
10. Check extensions; **singular extensions** (+ double/triple extensions, negative extensions, multi-cut).
11. "Improving" flag from static eval two plies back.

### 6.3 Move ordering (staged move picker)
Order: TT move → good captures (MVV + capture history, SEE ≥ threshold) → killers (1–2) / counter move → quiets by history → bad captures.
History tables:
- Butterfly / main history `[colour][from][to]`
- Capture history `[piece][to][captured]`
- Continuation history (1, 2, 4, 6 plies back) `[piece][to][piece][to]`
- Pawn-structure history (optional, SPRT)
- Gravity update: `h += bonus - h * |bonus| / MAX`.

### 6.4 Correction history
Correct static eval with learned residuals indexed by pawn key, non-pawn keys and continuation. Add it after the core is SPRT-validated; it is usually a solid gain.

### 6.5 Search parameters
Every tunable constant MUST be declared via a macro/registry so that it can be exposed as a UCI option in tuning builds (`TUNE` define) and stays `constexpr` in release builds. This makes SPSA tuning possible (§10.4) at zero cost to NPS.

---

## 7. Transposition table and shared-memory parallelism

### 7.1 Transposition table (shared by all threads)
- **Clusters of 3 entries in 32 bytes** (or another layout that fits one cache line), aligned to the cache line.
- Entry: key16, move16, value16, eval16, depth8, genBound8 (generation + PV flag + bound).
- Index with **multiply-high** (`(uint128)key * clusterCount >> 64`), never `%`.
- **Lock-free.** Races are tolerated by design: torn or stale entries are caught by the key16 check plus `pseudo_legal()` on the TT move. Use relaxed atomics or plain stores as SF does; document the reasoning and **don't add locks**.
- Replacement: depth + age (generation) + bound-type priority.
- **Prefetch** the TT cluster as soon as the child key is known (in `do_move`, before eval).
- **Large pages:** `madvise(MADV_HUGEPAGE)` on Linux, `VirtualAlloc` with `MEM_LARGE_PAGES` on Windows when the privilege exists. Fall back silently.
- Multithreaded clearing of the TT on `ucinewgame` / `Clear Hash`.
- `hashfull` reported in `info`.

### 7.2 Lazy SMP
- A **thread pool** created at `Threads` change, not per search. Threads sleep on a condition variable between searches.
- Each thread: its own `Position`, accumulator stack, history tables, killer tables, search stack. **Only the TT (and a stop flag/node counters) is shared.**
- Avoid false sharing: `alignas(64)` per-thread data; per-thread node counters, summed only for reporting.
- Helper threads search the same root with natural desynchronisation (and optionally depth skipping). Don't make it more complex than SF does.
- **Best-thread selection** by a vote weighted by depth and score (SF method), not just "main thread".
- **NUMA:** bind threads to nodes and replicate the NNUE weights per NUMA node when `Threads` > cores of one node. Phase 3 item, measured.

### 7.3 Thread scaling acceptance
- `speedtest` (or `bench` with N threads) reports NPS for 1, 2, 4, 8, … threads. Target: NPS scaling ≥ 0.9× linear up to physical cores.
- Elo scaling verified by SPRT: N threads vs 1 thread at the same time control (should be a strong gain), and "new SMP code vs old SMP code" at 8 threads.

---

## 8. Protocols

All protocol code is a thin frontend over one engine core (`Engine::set_position`, `go(Limits)`, `stop`, `set_option`). No search logic in frontends.

### 8.1 UCI (MUST, complete)
- Commands: `uci`, `isready`, `setoption`, `ucinewgame`, `position startpos|fen ... moves ...`, `go` (`wtime btime winc binc movestogo depth nodes mate movetime infinite ponder searchmoves`), `stop`, `ponderhit`, `quit`.
- Options: `Hash`, `Threads`, `MultiPV`, `Ponder`, `Move Overhead`, `EvalFile`, `UCI_Chess960`, `Clear Hash`, `UCI_ShowWDL`, `SyzygyPath` (phase 3).
- `info depth seldepth multipv score cp|mate [lowerbound|upperbound] nodes nps hashfull tbhits time pv`. Throttle `currmove` output (only after ~3 s).
- Non-standard but REQUIRED: `bench [hash] [threads] [depth] [fenfile]`, `perft <d>`, `d` (display board + FEN + key + eval), `eval` (per-net trace), `speedtest`.
- `bench` with no arguments MUST be **deterministic single-threaded** and end with these lines (Fishtest/OpenBench parse them):
  ```
  Total time (ms) : <ms>
  Nodes searched  : <nodes>
  Nodes/second    : <nps>
  ```
  and also a line `<nodes> nodes <nps> nps` for OpenBench compatibility.

### 8.2 XBoard / CECP v2 (MUST, enough for real GUIs and tournaments)
- Handshake: `xboard`, `protover 2` → `feature ping=1 setboard=1 usermove=1 time=1 draw=0 sigint=0 sigterm=0 reuse=1 analyze=1 colors=0 nps=0 memory=1 smp=1 myname="..." variants="normal,fischerandom" done=1`.
- Commands: `new`, `force`, `go`, `playother`, `usermove <move>`, `setboard <fen>`, `level`, `st`, `sd`, `time`, `otim`, `post`/`nopost`, `ping N` → `pong N`, `result`, `undo`, `remove`, `hard`/`easy` (pondering), `analyze`/`exit`/`.`, `memory`, `cores`, `quit`.
- Thinking output: `<ply> <score> <time-centiseconds> <nodes> <pv>`.
- Engine plays moves as `move e2e4`. Game-end claims via `1-0 {reason}` etc.
- **Test with a script that drives both protocols** through a full game, including clock handling and analyze mode.

---

## 9. Correctness and performance harness

### 9.1 Debug build
`make debug` with `-O1 -g -fsanitize=address,undefined` and asserts on. Run a 10-second search on 50 positions and the perft suite under sanitizers in CI. `-fsanitize=thread` build for SMP code.

### 9.2 Hash/state consistency
In debug builds, after each `do_move`/`undo_move`, recompute Zobrist keys and the NNUE accumulator from scratch and assert equality.

### 9.3 NNUE correctness
`/tests/eval_fens.epd`: ≥ 1,000 varied FENs with the expected eval from the **pinned reference Stockfish build** (`eval` command output). The engine must match exactly (phase 2), or within the documented rounding (phase 1).

### 9.4 NPS measurement protocol (MUST)
- Machine idle, CPU governor `performance`, turbo state documented, thread pinned (`taskset`).
- `bench` run **≥ 5 times**; report **median and spread**.
- Compare A/B builds **interleaved** (A, B, A, B, …) to cancel thermal drift.
- An NPS change smaller than the spread is "no change".
- Use `perf stat` / `perf record` to justify hot-path changes (IPC, cache misses, branch misses).

---

## 10. Strength testing (the only judge)

### 10.1 Tools
- **fastchess** (preferred) or **cutechess-cli** for local SPRT.
- Opening book: an unbalanced human-opening EPD book (e.g. the UHO books used by Stockfish testing), not the startposition.

### 10.2 SPRT procedure
- Elo model: **normalized Elo**, pentanomial statistics.
- **Gainer** patches: `elo0 = 0, elo1 = 2` (nElo). **Simplifications / non-regression:** `elo0 = -1.75, elo1 = 0.25`. (These are the usual Stockfish-style bounds; adjust for our smaller hardware budget, e.g. `[0, 5]` early on when the engine is weak and gains are large.)
- `alpha = beta = 0.05`.
- **STC** 10+0.1 (1 thread, 16 MB hash), then **LTC** 60+0.6 for patches that pass STC.
- SMP patches: 5+0.05 at 8 threads.

### 10.3 Commit discipline (MUST)
- One logical change per commit.
- **Every commit message ends with `Bench: <nodes>`**, the deterministic node count of `bench`. Fishtest-style frameworks use it to verify that the right binary was built. Functional changes change it; pure speed-ups and refactors MUST NOT.
- Pure speed patches: same bench signature + NPS protocol (§9.4) + a non-regression SPRT.

### 10.4 Cross-platform decision rule (Intel Mac vs Apple Silicon)

A change is accepted **only** if it passes one of these two routes:

**Route 1: proven-equivalent speed-up (the only "can't lose" case)**
- The `bench` node count is **identical** on both platforms before and after. The search visits exactly the same nodes, so the change cannot alter a move choice at fixed depth/nodes.
- NPS improves by more than the measured spread on at least one platform (§9.4) and is **not slower beyond the spread** on the other.
- If it helps one platform and hurts the other, put it **behind that platform's ARCH `#if`** so that each platform keeps its fastest version. Then it passes Route 1 on both.
- No SPRT required. A quick non-regression SPRT is optional.

**Route 2: anything that changes the search or eval (bench signature changes)**
- Must pass SPRT: gainer bounds at STC, then LTC, run on the Apple Silicon machine.
- If the change has a platform-dependent cost (extra eval work, bigger tables, more memory traffic), it must **also** pass a non-regression SPRT (`[-1.75, 0.25]` or the bounds currently used) on the Intel Mac at STC.
- If it can't pass on Intel, the agent must stop and report the numbers. The human decides; the agent never ships it silently.

**Not accepted as evidence:** a fixed number of games without error bars, "it feels stronger", Lichess rating changes, or NPS alone for a change that alters the bench signature.

Note: SPRT is a statistical test, not a guarantee. With `alpha = beta = 0.05` about 1 in 20 accepted patches can be neutral or slightly bad. Route 1 is the only route with no statistical risk at fixed depth/nodes.

### 10.5 Parameter tuning
SPSA via the `TUNE` build (§6.5) on a self-hosted framework, or a local SPSA runner. Tuned values go back in as `constexpr`.

---

## 11. Fishtest-style framework. Read carefully.

- **The official Fishtest (tests.stockfishchess.org) only tests Stockfish.** We cannot submit our engine there.
- **Option A — self-hosted Fishtest** (`official-stockfish/fishtest`, open source): the server + workers can run on our own machines, but the worker is written around Stockfish's repo, Makefile and net-download logic. Expect to **adapt the worker** (build commands, net fetching, bench parsing). Doable, but real work.
- **Option B — OpenBench** (`AndyGrant/OpenBench`): built **for engines other than Stockfish**. It supports SPRT, SPSA tuning, multiple engines, NNUE net hosting. It needs: a Makefile with `EXE=`/`CXX=` support, a `bench` command that prints `<nodes> nodes <nps> nps`, and the `Bench:` signature in commits. **This is the recommended default.**
- The engine MUST satisfy the requirements of **both** (Makefile interface §2, `bench` output §8.1, `Bench:` commit lines §10.3), so the choice can be made later.
- Until a framework runs, use local fastchess SPRT with the same bounds.

---

## 12. Hot-path optimisation checklist

- [ ] No heap allocation, exceptions, virtual calls, `std::function` or `std::map` inside search/eval.
- [ ] Node type and side to move as **template parameters** where it removes branches.
- [ ] `[[likely]]`/`[[unlikely]]` only where `perf` shows branch misses.
- [ ] TT prefetch on `do_move`; NNUE weights for the first layer stay hot in L2.
- [ ] Search stack (`Stack ss[MAX_PLY + 10]`) on the thread's stack, with `ss-7` access for continuation history.
- [ ] `popcount`/`tzcnt`/`pext` via intrinsics with portable fallbacks.
- [ ] Check node count / stop flag every N nodes (e.g. 1024), not every node; time checks only from the main thread.
- [ ] PGO build for every release and every NPS comparison.
- [ ] `constexpr` LMR table and attack tables computed at startup (or compile time where cheap).
- [ ] Avoid `std::vector` in move lists: fixed `std::array<ExtMove, 256>`.
- [ ] Measure before and after. Every optimisation PR includes `bench` NPS numbers per §9.4.

---

## 13. Time management
- Base: `optimum` and `maximum` time from remaining time, increment, `movestogo`, `Move Overhead`.
- Scale the stop time by: best-move stability, score drop vs previous iterations, fraction of nodes spent on the best move.
- Never lose on time: hard stop at `maximum`; test with 1+0.01 games (0 time losses allowed in 1,000 games).
- Pondering: `go ponder` / `ponderhit` (UCI), `hard` (XBoard).

---

## 14. Milestones (each one has an exit criterion)

| # | Milestone | Exit criterion |
|---|---|---|
| M0 | Build system, types, bitboards, magics/PEXT | Builds on GCC + Clang, all ARCH targets |
| M1 | Position + legal movegen | Full perft suite passes (incl. 960); movegen Mnps recorded |
| M2 | UCI frontend + simple alpha-beta + PSQT eval | Plays legal games via fastchess, no crashes in 1,000 games |
| M3 | HalfKP NNUE (phase 1) + incremental accumulator + SIMD | Eval matches reference on eval_fens; NPS baseline recorded |
| M4 | PVS, ID, aspiration, qsearch, TT, staged move picker | SPRT vs M2: large gain (expect hundreds of Elo) |
| M5 | Pruning/reductions/extensions list §6.2, one SPRT each | Each item passes its SPRT or is dropped with a note |
| M6 | Lazy SMP, thread pool, large pages | §7.3 scaling targets + SMP SPRT |
| M7 | XBoard frontend | Protocol script passes; full games in an XBoard GUI |
| M8 | Phase-2 NNUE (pinned SF architecture) | Exact eval match; SPRT gain vs HalfKP build |
| M9 | Framework (OpenBench or self-hosted Fishtest) | One full SPRT run end to end on the framework |
| M10 | Correction history, SPSA tuning, Syzygy, NUMA | Each item SPRT-validated |

**Order matters:** don't start M5 before M4 is solid. Don't optimise NPS before M3 is correct.

---

## 15. Agent working rules

1. Work milestone by milestone. At the end of each milestone, produce a short report: what was built, tests run, bench signature, NPS (median ± spread), SPRT results.
2. If a requirement here conflicts with measured reality (e.g. copy-make is faster), follow the measurement and **write down the numbers**.
3. Don't copy Stockfish source code without keeping its GPL-3 headers and attribution. Prefer writing our own code; read SF for reference on **formats** (the net file) and **ideas**.
4. Never weaken a test to make it pass. Never change the reference perft/eval files.
5. Ask the human before: changing the licence, switching the default net, or adding a dependency.
6. If something requested is impossible or a bad trade-off, say so plainly and give the closest workable option.

---

## Appendix A — Reference sources (read, don't guess)
- Stockfish source at the pinned tag: `nnue/`, `search.cpp`, `movepick.cpp`, `tt.cpp`, `thread.cpp`, `timeman.cpp`.
- Chess Programming Wiki: PVS, LMR, Null Move Pruning, Singular Extensions, Lazy SMP, Magic Bitboards, NNUE.
- `nnue-pytorch` docs (official-stockfish): the clearest written description of NNUE feature sets and quantisation.
- OpenBench README / wiki: engine integration requirements.
- fastchess README: SPRT options, pentanomial output.
