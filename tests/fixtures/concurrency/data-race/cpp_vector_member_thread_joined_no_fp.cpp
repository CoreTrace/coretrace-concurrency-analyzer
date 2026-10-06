// SPDX-License-Identifier: Apache-2.0
// The vector is the first member of a local object; a thread moved into it is joined by the loop
// over that member before main writes: nothing runs beside that write.
// Expected: no diagnostic.
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
};

int main()
{
    Pool pool;
    pool.threads.push_back(std::thread(worker));
    for (auto& thread : pool.threads)
        thread.join();
    shared += 2;
    return shared;
}
