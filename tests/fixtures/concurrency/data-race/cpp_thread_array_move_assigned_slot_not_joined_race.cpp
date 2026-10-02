// SPDX-License-Identifier: Apache-2.0
// The thread is moved into slot 1, and the loop joins slot 0 alone: the worker may still run
// when main writes after the loop (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::thread threads[2];
    threads[1] = std::thread(worker);
    threads[0] = std::thread([] {});
    for (int i = 0; i < 1; ++i)
        threads[i].join();
    shared += 2;
    threads[1].join();
    return shared;
}
