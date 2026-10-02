// SPDX-License-Identifier: Apache-2.0
// Two std::threads constructed in an array are joined by a range-for loop before main writes: nothing
// runs beside that write (#161).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void worker() { shared += 1; }
static void other() {}

int main()
{
    std::thread threads[2] = {std::thread(worker), std::thread(other)};
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    return shared;
}
