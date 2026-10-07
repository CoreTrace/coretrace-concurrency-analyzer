// SPDX-License-Identifier: Apache-2.0
// main writes before joining the threads a helper moved into the vector: they still run.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread(reader));
}

int main()
{
    std::vector<std::thread> threads;
    add(threads);
    shared = 1;
    for (auto& thread : threads)
        thread.join();
    return shared;
}
