// SPDX-License-Identifier: Apache-2.0
// The initialization is safe, but what the threads do with the object afterwards is not: they
// all increment the same field with no lock.
#include <thread>
#include <vector>

struct Counter
{
    int hits;
    Counter() : hits(0) {}
};

static int next_ticket()
{
    static Counter instance;
    return instance.hits++;
}

static void worker()
{
    next_ticket();
}

int main()
{
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++)
        threads.emplace_back(worker);
    for (std::thread& thread : threads)
        thread.join();
    return 0;
}
