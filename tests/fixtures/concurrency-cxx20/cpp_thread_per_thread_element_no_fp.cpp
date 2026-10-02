// SPDX-License-Identifier: Apache-2.0
// std::thread version: thread i writes only results[i]; main reads after joining every thread
// (#108).
// Expected: no diagnostic.
#include <thread>
static int results[4];
static void worker(int* slot)
{
    *slot = 42;
}
int main()
{
    std::thread threads[4];
    for (int i = 0; i < 4; ++i)
        threads[i] = std::thread(worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        threads[i].join();
    return results[0];
}
