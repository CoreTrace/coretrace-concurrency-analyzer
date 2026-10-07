// SPDX-License-Identifier: Apache-2.0
// Each round hands the vector to the helper, then starts a reader and a writer into it: the two
// threads a round starts run together, the next round's helper call joining them.
// Expected: one data race, first against second.
#include <thread>
#include <vector>

static int shared;

static void first()
{
    int seen = shared;
    (void)seen;
}

static void second()
{
    shared = 2;
}

static void joinAll(std::vector<std::thread>& threads)
{
    for (auto& thread : threads)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    for (int round = 0;; ++round)
    {
        joinAll(threads);
        threads.clear();
        if (round == 2)
            break;
        threads.emplace_back(std::thread(first));
        threads.emplace_back(std::thread(second));
    }
    return 0;
}
