// SPDX-License-Identifier: Apache-2.0
// A swap at the head of the loop dominates the store of each handle, yet runs again between the
// first round's store and the joins: it parks that round's reader in a slot never joined before
// main writes (#206).
// Expected: one data race, main against reader.
#include <thread>
#include <utility>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

int main()
{
    std::thread threads[2];
    std::thread spares[2];
    spares[1] = std::thread(idle);
    for (int i = 0; i < 2; ++i)
    {
        std::swap(threads[0], spares[i]);
        threads[i] = std::thread(reader);
    }
    for (std::thread& thread : threads)
        thread.join();
    shared = 1;
    for (std::thread& spare : spares)
        if (spare.joinable())
            spare.join();
    return shared;
}
