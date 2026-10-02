// SPDX-License-Identifier: Apache-2.0
// The thread move-assigned into the array's slot is joined by the range-for before main writes
// (#162).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::thread threads[1];
    threads[0] = std::thread(worker);
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    return shared;
}
