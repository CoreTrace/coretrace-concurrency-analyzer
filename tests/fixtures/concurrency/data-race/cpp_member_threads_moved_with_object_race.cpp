// SPDX-License-Identifier: Apache-2.0
// Each round moves the whole object holding the vector into another vector: the reader left in the
// member moves away unjoined, the member is left empty, and the reader still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <utility>
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

int main()
{
    std::vector<Pool> pools;
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread([] {}));
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pools.push_back(std::move(pool));
    }
    shared = 1;
    for (auto& moved : pools)
        for (auto& thread : moved.threads)
            thread.join();
    return shared;
}
