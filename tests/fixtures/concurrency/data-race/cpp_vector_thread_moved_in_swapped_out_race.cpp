// SPDX-License-Identifier: Apache-2.0
// The vector's thread is swapped into another vector before the loop, which then joins nothing:
// the worker still runs when main writes after the loop, and is joined only afterwards (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::vector<std::thread> threads;
    std::vector<std::thread> others;
    threads.push_back(std::thread(worker));
    threads.swap(others);
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    for (auto& thread : others)
        thread.join();
    return shared;
}
