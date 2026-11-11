/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#include "thread.h"

#include <algorithm>

#include "movegen.h"

namespace Nest {

Thread::Thread(ThreadPool& pool, TranspositionTable& tt, size_t index) :
    worker(std::make_unique<Search::Worker>(pool, tt, index)),
    stdThread(&Thread::idle_loop, this) {
    wait_until_idle();
}

Thread::~Thread() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        exit = true;
    }
    cv.notify_all();
    stdThread.join();
}

void Thread::run(std::function<void()> f) {
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return !busy; });
        job  = std::move(f);
        busy = true;
    }
    cv.notify_all();
}

void Thread::wait_until_idle() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return !busy; });
}

void Thread::idle_loop() {
    while (true)
    {
        std::function<void()> f;
        {
            std::unique_lock<std::mutex> lock(mutex);
            busy = false;
            cv.notify_all();
            cv.wait(lock, [&] { return busy || exit; });
            if (exit)
                return;
            f = std::move(job);
        }
        if (f)
            f();
    }
}

void ThreadPool::release() {
    if (!threads.empty())
    {
        wait_for_search_finished();
        threads.clear();
    }
}

void ThreadPool::set(size_t count, TranspositionTable& tt) {
    release();
    for (size_t i = 0; i < count; ++i)
        threads.push_back(std::make_unique<Thread>(*this, tt, i));
}

void ThreadPool::clear() {
    for (auto& th : threads)
        th->worker->clear();
}

void ThreadPool::start_thinking(
  const Position& pos, StateList& states, const Search::Limits& limits) {
    wait_for_search_finished();

    stop          = false;
    ponder        = limits.ponder;
    increaseDepth = true;

    std::vector<Search::RootMove> rootMoves;
    for (const auto& m : MoveList<LEGAL>(pos))
        if (limits.searchmoves.empty()
            || std::count(limits.searchmoves.begin(), limits.searchmoves.end(), m.move))
            rootMoves.emplace_back(m.move);

    // Each worker gets its own copy of the root position. The copies share
    // the (read-only) state history, which repetition detection needs.
    const std::string fen = pos.fen();
    for (auto& th : threads)
    {
        Search::Worker& w = *th->worker;
        w.limits          = limits;
        w.nodes           = 0;
        w.bestMoveChanges = 0;
        w.rootDepth = w.completedDepth = w.selDepth = 0;
        w.rootMoves                                 = rootMoves;
        w.rootPos.set(fen, pos.is_chess960(), &w.rootState);
        w.rootState = states.back();
    }

    threads.front()->run([this] { main_worker()->start_searching(); });
}

void ThreadPool::start_helpers() {
    for (size_t i = 1; i < threads.size(); ++i)
    {
        Search::Worker* w = threads[i]->worker.get();
        threads[i]->run([w] { w->start_searching(); });
    }
}

void ThreadPool::wait_for_helpers() {
    for (size_t i = 1; i < threads.size(); ++i)
        threads[i]->wait_until_idle();
}

void ThreadPool::wait_for_search_finished() {
    for (auto& th : threads)
        th->wait_until_idle();
}

uint64_t ThreadPool::nodes_searched() const {
    uint64_t n = 0;
    for (const auto& th : threads)
        n += th->worker->nodes.load(std::memory_order_relaxed);
    return n;
}

uint64_t ThreadPool::collect_best_move_changes() {
    uint64_t n = 0;
    for (const auto& th : threads)
        n += th->worker->bestMoveChanges.exchange(0, std::memory_order_relaxed);
    return n;
}

// Chooses whose result to play. Each thread votes for its best move with a
// weight that grows with its score and completed depth; the move with the
// most votes wins, and among its supporters the deepest/best thread.
Search::Worker* ThreadPool::best_worker() const {
    Search::Worker* best = main_worker();
    if (threads.size() == 1)
        return best;

    Value minScore = VALUE_INFINITE;
    for (const auto& th : threads)
        minScore = std::min(minScore, th->worker->rootMoves[0].score);

    auto weight = [&](const Search::Worker* w) {
        return int64_t(w->rootMoves[0].score - minScore + 14) * int64_t(w->completedDepth);
    };
    auto votes_for = [&](Move m) {
        int64_t v = 0;
        for (const auto& th : threads)
            if (th->worker->rootMoves[0].pv[0] == m)
                v += weight(th->worker.get());
        return v;
    };

    for (const auto& th : threads)
    {
        Search::Worker* w         = th->worker.get();
        const Value     bestScore = best->rootMoves[0].score;
        const Value     score     = w->rootMoves[0].score;

        if (bestScore >= VALUE_TB_WIN_IN_MAX_PLY || bestScore <= VALUE_TB_LOSS_IN_MAX_PLY)
        {
            // With a decisive score, take the fastest win / slowest loss.
            if (score > bestScore)
                best = w;
            continue;
        }

        if (score >= VALUE_TB_WIN_IN_MAX_PLY || (score > VALUE_TB_LOSS_IN_MAX_PLY
            && (votes_for(w->rootMoves[0].pv[0]) > votes_for(best->rootMoves[0].pv[0])
                || (w->rootMoves[0].pv[0] == best->rootMoves[0].pv[0] && weight(w) > weight(best)
                    && w->rootMoves[0].pv.size() > 2))))
            best = w;
    }
    return best;
}

}  // namespace Nest
