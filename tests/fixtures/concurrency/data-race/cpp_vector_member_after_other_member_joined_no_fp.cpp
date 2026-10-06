// SPDX-License-Identifier: Apache-2.0
// The vector is a member of a local object, after another member main writes; the loop over the
// vector joins its thread before main writes shared.
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
    int started = 0;
    std::vector<std::thread> threads;
};

int main()
{
    Pool pool;
    pool.threads.push_back(std::thread(worker));
    pool.started += 1;
    for (auto& thread : pool.threads)
        thread.join();
    shared += 2;
    return shared + pool.started;
}
