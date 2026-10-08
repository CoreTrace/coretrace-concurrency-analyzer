// SPDX-License-Identifier: Apache-2.0
// The sibling member swaps contents with the vector the loop empties: the reader left in the
// vector moves to the sibling unjoined, and still runs when main writes.
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

int main()
{
    Pool pool;
    pool.threads.emplace_back(std::thread(reader));
    pool.threads.emplace_back(std::thread([] {}));
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.others.swap(pool.threads);
    }
    shared = 1;
    for (auto& thread : pool.others)
        thread.join();
    return shared;
}
