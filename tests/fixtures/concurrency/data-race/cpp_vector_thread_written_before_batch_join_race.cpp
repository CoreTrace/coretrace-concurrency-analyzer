// SPDX-License-Identifier: Apache-2.0
// In each round, main writes after moving the round's thread into the vector and before joining
// it: that thread still runs.
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
    for (int batch = 0; batch < 2; ++batch)
    {
        std::vector<std::thread> threads;
        threads.push_back(std::thread(reader));
        shared += 2;
        for (auto& thread : threads)
            thread.join();
    }
    return shared;
}
