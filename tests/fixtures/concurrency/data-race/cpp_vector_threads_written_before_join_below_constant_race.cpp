// SPDX-License-Identifier: Apache-2.0
// main writes before the loop joining the vector below 3: its threads still run.
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
    for (int i = 0; i < 3; ++i)
        threads.push_back(std::thread(reader));
    shared = 1;
    for (int i = 0; i < 3; ++i)
        threads[i].join();
    return shared;
}
