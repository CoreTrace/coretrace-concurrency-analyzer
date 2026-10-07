// SPDX-License-Identifier: Apache-2.0
// The helper moves its thread into the vector, then moves it out to a global and drops it from the
// vector: the loop joining the vector ends nothing, and the thread still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static std::thread kept;

static void add(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread(reader));
    kept = std::move(threads.back());
    threads.pop_back();
}

int main()
{
    std::vector<std::thread> threads;
    add(threads);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    kept.join();
    return shared;
}
