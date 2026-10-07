// SPDX-License-Identifier: Apache-2.0
// Each round moves two threads into its own vector and joins them: the two threads of a round
// write shared together, but no thread runs when main writes after the loop.
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
    for (int batch = 0; batch < 2; ++batch)
    {
        std::vector<std::thread> threads;
        threads.push_back(std::thread(worker));
        threads.push_back(std::thread(worker));
        for (auto& thread : threads)
            thread.join();
    }
    shared += 2;
    return shared;
}
