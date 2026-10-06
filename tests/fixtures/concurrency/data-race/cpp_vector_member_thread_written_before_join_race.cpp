// SPDX-License-Identifier: Apache-2.0
// main writes before the loop that joins the member vector: its thread still runs.
// Expected: one data race, main against worker.
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
    shared += 2;
    for (auto& thread : pool.threads)
        thread.join();
    return shared;
}
