// SPDX-License-Identifier: Apache-2.0
// Two threads moved into a member vector write shared together; the loop over the member joins
// both before main writes. The workers race with each other, main with neither.
// Expected: one data race, worker against worker.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

struct Pool
{
    std::vector<std::thread> threads;
};

int main()
{
    Pool pool;
    pool.threads.push_back(std::thread(worker));
    pool.threads.push_back(std::thread(worker));
    for (auto& thread : pool.threads)
        thread.join();
    shared += 2;
    return shared;
}
