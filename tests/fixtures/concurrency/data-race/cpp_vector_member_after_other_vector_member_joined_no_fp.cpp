// SPDX-License-Identifier: Apache-2.0
// The thread vector follows another vector member, which main appends to and reads; the loop over
// the thread vector joins its thread before main writes. The other member's uses do not reach the
// thread vector.
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
    std::vector<int> sizes;
    std::vector<std::thread> threads;
};

int main()
{
    Pool pool;
    pool.sizes.push_back(1);
    pool.threads.push_back(std::thread(worker));
    for (auto& thread : pool.threads)
        thread.join();
    shared += pool.sizes.back();
    return shared;
}
