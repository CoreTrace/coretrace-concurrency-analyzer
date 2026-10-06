// SPDX-License-Identifier: Apache-2.0
// The iterator loop starts past the first element: the thread moved in first is never joined
// through it->join(), and still runs when main writes.
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
    threads.push_back(std::thread([] {}));
    for (auto it = threads.begin() + 1; it != threads.end(); ++it)
        it->join();
    shared += 2;
    threads.front().join();
    return shared;
}
