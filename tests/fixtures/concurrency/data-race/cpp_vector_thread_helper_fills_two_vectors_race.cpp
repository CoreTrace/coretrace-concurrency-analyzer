// SPDX-License-Identifier: Apache-2.0
// The helper moves a reader into the first vector and an idle thread into the second: main joins
// the second only, and the reader still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& readers, std::vector<std::thread>& idle)
{
    readers.emplace_back(std::thread(reader));
    idle.emplace_back(std::thread([] {}));
}

int main()
{
    std::vector<std::thread> readers;
    std::vector<std::thread> threads;
    add(readers, threads);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    for (auto& thread : readers)
        thread.join();
    return shared;
}
