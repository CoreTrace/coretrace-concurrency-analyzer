// SPDX-License-Identifier: Apache-2.0
// main writes before the loop joining and popping the vector's threads: they still run.
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
    threads.emplace_back(std::thread(reader));
    shared = 1;
    while (!threads.empty())
    {
        threads.back().join();
        threads.pop_back();
    }
    return shared;
}
