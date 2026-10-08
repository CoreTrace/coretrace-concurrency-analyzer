// SPDX-License-Identifier: Apache-2.0
// The helper pops the member vector empty, joining its last thread each round, and is also handed
// that vector as the reference a function returns: through it, each round detaches the first
// thread and erases it. The reader is never joined and still runs when main writes.
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
};

static std::vector<std::thread>& identity(std::vector<std::thread>& threads)
{
    return threads;
}

static void drain(Pool& pool, std::vector<std::thread>& alias)
{
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        if (!alias.empty())
        {
            alias.front().detach();
            alias.erase(alias.begin());
        }
    }
}

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread([] {}));
    drain(pool, identity(pool.threads));
    shared = 1;
    return shared;
}
