// SPDX-License-Identifier: Apache-2.0
// main writes after moving the thread into the array and before the range-for joins it: the
// worker runs beside that write (#162).
// Expected: one data race, main's write against the worker's.
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
    shared += 2;
    for (auto& thread : threads)
        thread.join();
    return shared;
}
