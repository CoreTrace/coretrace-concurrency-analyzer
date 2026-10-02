// SPDX-License-Identifier: Apache-2.0
// The thread moved into the vector is moved out again before the loop, which then joins nothing:
// the worker still runs when main writes after the loop, and is joined only afterwards (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::vector<std::thread> threads;
    threads.push_back(std::thread(worker));
    std::thread moved = std::move(threads.back());
    threads.pop_back();
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    moved.join();
    return shared;
}
