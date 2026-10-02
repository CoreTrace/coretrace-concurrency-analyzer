// SPDX-License-Identifier: Apache-2.0
// Each round below a bound read from the arguments moves its thread into its own slot, and a loop
// below the same bound joins every slot before main writes an id (#162).
// Expected: no diagnostic.
#include <thread>

static void worker(const int* id)
{
    volatile int seen = *id;
    (void)seen;
}

int main(int argc, char**)
{
    int ids[4] = {0, 1, 2, 3};
    std::thread threads[4];
    const int count = argc < 4 ? argc : 4;
    for (int i = 0; i < count; ++i)
        threads[i] = std::thread(worker, &ids[i]);
    for (int k = 0; k < count; ++k)
        threads[k].join();
    ids[0] = 4;
    return ids[0];
}
