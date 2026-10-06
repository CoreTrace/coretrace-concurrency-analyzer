// SPDX-License-Identifier: Apache-2.0
// Three threads moved into the vector write shared together; the loop below 3 joins them all
// before main writes. The workers race with each other, main with none.
// Expected: one data race, worker against worker.
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
    for (int i = 0; i < 3; ++i)
        threads.push_back(std::thread(worker));
    for (int i = 0; i < 3; ++i)
        threads[i].join();
    shared = 1;
    return shared;
}
