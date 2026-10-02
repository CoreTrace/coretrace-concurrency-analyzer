// SPDX-License-Identifier: Apache-2.0
// A helper keeps the array's address before the worker is moved in, and a second helper later
// swaps the worker out through it: the range-for joins an idle thread, and the worker still runs
// when main writes after the loop (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>

static int shared;
static std::thread* kept;
static std::thread spare;

static void worker()
{
    shared += 1;
}

static void idle() {}

static void keep(std::thread* threads)
{
    kept = threads;
}

static void exchange()
{
    spare = std::thread(idle);
    spare.swap(*kept);
}

int main()
{
    std::thread threads[1];
    keep(threads);
    threads[0] = std::thread(worker);
    exchange();
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    spare.join();
    return shared;
}
