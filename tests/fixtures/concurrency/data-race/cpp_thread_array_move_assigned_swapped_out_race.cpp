// SPDX-License-Identifier: Apache-2.0
// The worker moved into the array is swapped out for an idle thread before the range-for: the
// loop joins the idle thread, and the worker still runs when main writes after it (#162).
// Expected: one data race, main's write against the worker's.
#include <thread>

static int shared;

static void worker()
{
    shared += 1;
}

static void idle() {}

int main()
{
    std::thread threads[1];
    threads[0] = std::thread(worker);
    std::thread other(idle);
    other.swap(threads[0]);
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    other.join();
    return shared;
}
