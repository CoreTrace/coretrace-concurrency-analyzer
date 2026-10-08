// SPDX-License-Identifier: Apache-2.0
// Each round hands the whole object to a helper, which swaps the vector's first thread out for an
// idle one and detaches it: the reader swapped out still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

struct Pool
{
    std::vector<int> sizes;
    std::vector<std::thread> threads;
};

static void takeOut(Pool& pool)
{
    if (pool.threads.empty())
        return;
    std::thread idle([] {});
    pool.threads.front().swap(idle);
    idle.detach();
}

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread([] {}));
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        takeOut(pool);
    }
    shared = 1;
    return shared;
}
