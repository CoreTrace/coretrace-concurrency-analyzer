// SPDX-License-Identifier: Apache-2.0
// Counters::bump() increments values[index] of its object. main calls counters.bump(0) and the
// thread counters.bump(1): the object is the same, the elements distinct (#159).
// Expected: no diagnostic.
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
    counters.bump(0);
    worker.join();
    return counters.values[0] + counters.values[1];
}
