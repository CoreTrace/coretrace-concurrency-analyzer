// SPDX-License-Identifier: Apache-2.0
// The loop empties one member vector of threads while it moves readers into a sibling member of
// the same type: the sibling's readers still run when main writes.
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
    pool.threads.emplace_back(std::thread([] {}));
    while (!pool.threads.empty())
    {
        pool.threads.back().join();
        pool.threads.pop_back();
        pool.others.emplace_back(std::thread(reader));
    }
    shared = 1;
    for (auto& thread : pool.others)
        thread.join();
    return shared;
}
