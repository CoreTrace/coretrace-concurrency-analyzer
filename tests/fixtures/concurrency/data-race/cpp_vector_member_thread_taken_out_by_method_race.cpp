// SPDX-License-Identifier: Apache-2.0
// A method of the object moves the thread out of its member vector before the loop joins the
// vector: the loop never joins it, and it still runs when main writes.
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
    std::thread takeLast()
    {
        std::thread last = std::move(threads.back());
        threads.pop_back();
        return last;
    }
};

int main()
{
    Pool pool;
    pool.threads.push_back(std::thread(worker));
    std::thread taken = pool.takeLast();
    for (auto& thread : pool.threads)
        thread.join();
    shared += 2;
    taken.join();
    return shared;
}
