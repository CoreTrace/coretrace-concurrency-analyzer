// SPDX-License-Identifier: Apache-2.0
// Two std::threads constructed in an array are joined by an index loop before main writes (#161).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void worker() { shared += 1; }
static void other() {}

int main()
{
    std::thread threads[2] = {std::thread(worker), std::thread(other)};
    for (int i = 0; i < 2; i++)
        threads[i].join();
    shared += 2;
    return shared;
}
