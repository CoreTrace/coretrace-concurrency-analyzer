// SPDX-License-Identifier: Apache-2.0
// Each round hands the vector to the helper, then starts a thread into it and writes: the thread
// started in a round runs during that round's write, the next round's helper call joining it.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
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
        threads.emplace_back(std::thread(reader));
        shared = round;
    }
    return shared;
}
