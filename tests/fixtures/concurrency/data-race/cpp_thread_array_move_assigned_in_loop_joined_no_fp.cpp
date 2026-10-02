// SPDX-License-Identifier: Apache-2.0
// Each round moves its thread into its own slot, and the range-for joins every slot before main
// writes the counts (#162).
// Expected: no diagnostic.
#include <thread>

static void worker(const int* id)
{
    volatile int seen = *id;
    (void)seen;
}

int main()
{
    int ids[2] = {0, 1};
    std::thread threads[2];
    for (int i = 0; i < 2; ++i)
        threads[i] = std::thread(worker, &ids[i]);
    for (auto& thread : threads)
        thread.join();
    ids[0] = 2;
    return ids[0];
}
