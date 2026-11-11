/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "search.h"
#include "tt.h"

namespace Nest {

using StateList = std::deque<StateInfo>;

// An OS thread that sleeps between jobs. It owns one search worker.
class Thread {
   public:
    Thread(ThreadPool& pool, TranspositionTable& tt, size_t index);
    ~Thread();

    void run(std::function<void()> f);
    void wait_until_idle();

    std::unique_ptr<Search::Worker> worker;

   private:
    void idle_loop();

    std::mutex              mutex;
    std::condition_variable cv;
    std::function<void()>   job;
    bool                    exit = false, busy = true;
    std::thread             stdThread;
};

class ThreadPool {
   public:
    ~ThreadPool() { release(); }

    void   set(size_t count, TranspositionTable& tt);
    void   release();
    size_t size() const { return threads.size(); }
    void   clear();

    // Starts a search and returns at once; the main worker reports through
    // the callbacks. `states` must hold the game history of `pos`.
    void start_thinking(const Position& pos, StateList& states, const Search::Limits& limits);
    void start_helpers();
    void wait_for_helpers();
    void wait_for_search_finished();

    Search::Worker* main_worker() const { return threads.front()->worker.get(); }
    Search::Worker* best_worker() const;
    uint64_t        nodes_searched() const;
    uint64_t        collect_best_move_changes();

    std::atomic<bool> stop{false}, ponder{false}, increaseDepth{true};

    Search::Callbacks callbacks;
    Search::Settings  settings;

   private:
    std::vector<std::unique_ptr<Thread>> threads;
};

}  // namespace Nest
