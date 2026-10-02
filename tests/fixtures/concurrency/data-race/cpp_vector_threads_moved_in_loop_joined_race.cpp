// SPDX-License-Identifier: Apache-2.0
// Two threads started in a loop are moved into the vector, then joined by a loop over it. The
// workers race with each other; main's write follows every join and races with nothing (#162).
// Expected: one data race, the workers against each other.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i)
        threads.push_back(std::thread(worker));
    for (auto& thread : threads)
        thread.join();
    shared = 0;
    return shared;
}
