// SPDX-License-Identifier: Apache-2.0
// main writes before the iterator loop that joins the vector through it->join(): the thread
// moved into the vector still runs.
// Expected: one data race, main against worker.
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
    shared += 2;
    for (auto it = threads.begin(); it != threads.end(); ++it)
        it->join();
    return shared;
}
