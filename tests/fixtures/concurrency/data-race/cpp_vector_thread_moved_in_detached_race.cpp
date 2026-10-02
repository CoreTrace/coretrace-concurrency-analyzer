// SPDX-License-Identifier: Apache-2.0
// The loop over the vector detaches the thread moved into it instead of joining it: the worker may
// still run when main writes after the loop (#162).
// Expected: one data race, main's write against the worker's.
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
    threads.push_back(std::thread(worker));
    for (auto& thread : threads)
        thread.detach();
    shared += 2;
    return 0;
}
