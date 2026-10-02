// SPDX-License-Identifier: Apache-2.0
// main writes while the threads constructed in the array run, before the range-for loop joins them.
// Expected: one data race.
#include <thread>

static int shared;

static void worker() { shared += 1; }
static void other() {}

int main()
{
    std::thread threads[2] = {std::thread(worker), std::thread(other)};
    shared += 2;
    for (auto& thread : threads)
        thread.join();
    return shared;
}
