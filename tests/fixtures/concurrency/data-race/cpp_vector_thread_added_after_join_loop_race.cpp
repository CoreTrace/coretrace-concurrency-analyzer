// SPDX-License-Identifier: Apache-2.0
// The helper adds its thread after main's loop joined the vector: that thread still runs when main
// writes.
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
    for (auto& thread : threads)
        thread.join();
    add(threads);
    shared = 1;
    threads.back().join();
    return shared;
}
