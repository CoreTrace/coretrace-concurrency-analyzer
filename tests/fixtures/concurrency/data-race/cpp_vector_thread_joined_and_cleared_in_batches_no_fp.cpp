// SPDX-License-Identifier: Apache-2.0
// One vector serves every round: each round moves a thread in, joins the vector, then clears it.
// No two threads run at once, and none runs when main writes after the loop.
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
    std::vector<std::thread> threads;
    for (int batch = 0; batch < 2; ++batch)
    {
        threads.push_back(std::thread(reader));
        for (auto& thread : threads)
            thread.join();
        threads.clear();
    }
    shared += 2;
    return shared;
}
