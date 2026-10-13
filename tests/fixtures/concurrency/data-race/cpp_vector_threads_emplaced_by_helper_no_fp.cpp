// SPDX-License-Identifier: Apache-2.0
// A helper builds a thread in the vector it is handed with emplace_back; main joins every element
// before writing (#123, form h01).
// Expected: no diagnostic.
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
    threads.emplace_back(reader);
}

int main()
{
    std::vector<std::thread> threads;
    add(threads);
    add(threads);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
