// SPDX-License-Identifier: Apache-2.0
// main writes before the loop joining and popping the member vector beside a sibling member: the
// threads still run.
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

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    shared = 1;
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.sizes.push_back(1);
    }
    return shared;
}
