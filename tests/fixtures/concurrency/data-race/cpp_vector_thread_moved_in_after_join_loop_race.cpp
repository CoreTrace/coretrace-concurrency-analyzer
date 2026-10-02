// SPDX-License-Identifier: Apache-2.0
// The thread is moved into the vector after the join loop has run: that loop never saw it, and
// main's write runs beside it before the second loop joins it (#162).
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
    for (auto& thread : threads)
        thread.join();
    threads.push_back(std::thread(worker));
    shared += 2;
    for (auto& thread : threads)
        thread.join();
    return shared;
}
