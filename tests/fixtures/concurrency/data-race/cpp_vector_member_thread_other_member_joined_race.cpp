// SPDX-License-Identifier: Apache-2.0
// The object holds two vectors; the loop joins the other one, so the thread moved into the
// first still runs when main writes.
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
    std::vector<std::thread> others;
};

int main()
{
    Pool pool;
    pool.threads.push_back(std::thread(worker));
    pool.others.push_back(std::thread([] {}));
    for (auto& thread : pool.others)
        thread.join();
    shared += 2;
    pool.threads.front().join();
    return shared;
}
