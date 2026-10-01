// SPDX-License-Identifier: Apache-2.0
// Counters::bump() increments values[index] of its object. main and the thread both call
// counters.bump(1): the same element of the same object (#159).
// Expected: one data race on `counters`, main's call to bump() against the thread's.
#include <thread>

struct Counters
{
    int values[2] = {};

    void bump(int index)
    {
        values[index] += 1;
    }
};

static Counters counters;

static void run()
{
    counters.bump(1);
}

int main()
{
    std::thread worker(run);
    counters.bump(1);
    worker.join();
    return counters.values[1];
}
