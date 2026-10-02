// SPDX-License-Identifier: Apache-2.0
// Each round moves its thread into the slot the count names, then raises the count; the loop
// joins every slot below it before main writes an id (#162).
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
    int started = 0;
    for (int i = 0; i < 2; ++i)
        threads[started++] = std::thread(worker, &ids[i]);
    for (int k = 0; k < started; ++k)
        threads[k].join();
    ids[0] = 2;
    return ids[0];
}
