// SPDX-License-Identifier: Apache-2.0
// The loop before main's write joins another vector's thread: the worker, in the first vector,
// still runs when main writes, and is joined only afterwards (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

static void idle() {}

int main()
{
    std::vector<std::thread> threads;
    std::vector<std::thread> others;
    threads.push_back(std::thread(worker));
    others.push_back(std::thread(idle));
    for (auto& thread : others)
        thread.join();
    shared += 2;
    for (auto& thread : threads)
        thread.join();
    return shared;
}
