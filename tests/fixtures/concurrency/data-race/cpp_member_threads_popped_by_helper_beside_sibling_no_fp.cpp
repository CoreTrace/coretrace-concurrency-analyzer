// SPDX-License-Identifier: Apache-2.0
// A helper handed the object joins and pops its member vector until empty, appending to a sibling
// member each round: every thread is joined before main writes (#190).
// Expected: no diagnostic.
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

static void drain(Pool& pool)
{
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.sizes.push_back(1);
    }
}

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread(reader));
    drain(pool);
    shared = 1;
    return shared;
}
