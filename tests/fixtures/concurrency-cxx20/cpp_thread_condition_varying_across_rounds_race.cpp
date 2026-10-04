// SPDX-License-Identifier: Apache-2.0
// The test guarding the start is computed anew in each round: the thread round 0 starts still
// reads shared when round 1 takes the other branch and writes it (#113).
// Expected: one data race.
#include <thread>

static int shared;

static void worker()
{
    volatile int seen = shared;
    (void)seen;
}

static bool pick(int round)
{
    return round == 0;
}

int main()
{
    std::thread thread;
    for (int round = 0; round < 2; ++round)
    {
        const bool start = pick(round);
        if (start)
            thread = std::thread(worker);
        else
            shared += 2;
    }
    if (thread.joinable())
        thread.join();
    return 0;
}
