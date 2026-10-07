// SPDX-License-Identifier: Apache-2.0
// Each round of a loop moves a thread into its own vector and joins that vector before the next
// round: no two threads run at once, and none runs when main writes after the loop.
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    for (int batch = 0; batch < 2; ++batch)
    {
        std::vector<std::thread> threads;
        threads.push_back(std::thread(reader));
        for (auto& thread : threads)
            thread.join();
    }
    shared += 2;
    return shared;
}
