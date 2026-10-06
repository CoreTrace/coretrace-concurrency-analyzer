// SPDX-License-Identifier: Apache-2.0
// Each round joins the vector, then moves a new thread in: the next round's loop joins it, but the
// last round's thread is still running when main writes after the loop.
// Expected: one data race, main against reader.
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
        for (auto& thread : threads)
            if (thread.joinable())
                thread.join();
        threads.push_back(std::thread(reader));
    }
    shared += 2;
    for (auto& thread : threads)
        if (thread.joinable())
            thread.join();
    return shared;
}
