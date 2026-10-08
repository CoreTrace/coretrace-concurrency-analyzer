// SPDX-License-Identifier: Apache-2.0
// The helper emptying the member vector swaps a sibling member with a pointer the analysis cannot
// follow, which aliases that vector: the reader left in it moves to the sibling unjoined, and
// still runs when main writes.
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
    std::vector<std::thread> threads;
    std::vector<std::thread> others;
};

static std::vector<std::thread>& identity(std::vector<std::thread>& threads)
{
    return threads;
}

static void drain(Pool& pool, std::vector<std::thread>* alias)
{
    std::vector<std::thread> spare;
    if (alias == nullptr)
        alias = &spare;
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.others.swap(*alias);
    }
}

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread([] {}));
    drain(pool, &identity(pool.threads));
    shared = 1;
    for (auto& thread : pool.others)
        thread.join();
    return shared;
}
