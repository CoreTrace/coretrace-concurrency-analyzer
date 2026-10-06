// SPDX-License-Identifier: Apache-2.0
// A method hands the object back by reference, and main takes the thread out of the member
// vector through it before the loop joins the vector: the loop never joins that thread, and it
// still runs when main writes.
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
    Pool& self()
    {
        return *this;
    }
};

int main()
{
    Pool pool;
    pool.threads.push_back(std::thread(worker));
    std::thread taken = std::move(pool.self().threads.back());
    pool.self().threads.pop_back();
    for (auto& thread : pool.threads)
        thread.join();
    shared += 2;
    taken.join();
    return shared;
}
