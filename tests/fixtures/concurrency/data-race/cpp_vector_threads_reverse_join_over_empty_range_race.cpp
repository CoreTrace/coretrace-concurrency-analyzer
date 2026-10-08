// SPDX-License-Identifier: Apache-2.0
// The loop runs from rbegin() to rbegin(), an empty range: it joins nothing, and the reader still
// runs when main writes.
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
    for (auto it = threads.rbegin(); it != threads.rbegin(); ++it)
        it->join();
    shared = 1;
    threads.back().join();
    return shared;
}
