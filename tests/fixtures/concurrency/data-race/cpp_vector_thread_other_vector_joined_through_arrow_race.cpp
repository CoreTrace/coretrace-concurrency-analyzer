// SPDX-License-Identifier: Apache-2.0
// The iterator loop joins another vector's threads through it->join(): the thread moved into
// this vector still runs when main writes.
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
    std::vector<std::thread> others;
    threads.push_back(std::thread(worker));
    others.push_back(std::thread([] {}));
    for (auto it = others.begin(); it != others.end(); ++it)
        it->join();
    shared += 2;
    threads.front().join();
    return shared;
}
