// SPDX-License-Identifier: Apache-2.0
// Until the member vector is empty, each round joins its last thread and pops it, and appends to
// a sibling member: every thread is joined before main writes (#190).
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

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread(reader));
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.sizes.push_back(1);
    }
    shared = 1;
    return shared;
}
