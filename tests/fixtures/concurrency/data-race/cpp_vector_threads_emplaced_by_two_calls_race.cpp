// SPDX-License-Identifier: Apache-2.0
// Two emplace_back calls build two threads running the same entry: both run until the join
// loop, so they race with each other (#123).
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
    threads.emplace_back(worker);
    threads.emplace_back(worker);
    for (auto& thread : threads)
        thread.join();
    return shared;
}
