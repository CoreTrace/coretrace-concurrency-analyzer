// SPDX-License-Identifier: Apache-2.0
// Two threads moved into the vector write shared together; the iterator loop joins both through
// it->join() before main writes. The workers race with each other, main with neither.
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
    threads.push_back(std::thread(worker));
    threads.push_back(std::thread(worker));
    for (auto it = threads.begin(); it != threads.end(); ++it)
        it->join();
    shared += 2;
    return shared;
}
