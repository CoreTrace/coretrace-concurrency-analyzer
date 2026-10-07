// SPDX-License-Identifier: Apache-2.0
// main writes before handing the vector to the helper joining its threads: they still run.
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
    threads.emplace_back(std::thread(reader));
    shared = 1;
    joinAll(threads);
    return shared;
}
