// SPDX-License-Identifier: Apache-2.0
// The loop from rbegin() to rend() joins another vector's threads: the reader moved into this one
// still runs when main writes.
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
    std::vector<std::thread> others;
    threads.emplace_back(std::thread(reader));
    others.emplace_back(std::thread([] {}));
    for (auto it = others.rbegin(); it != others.rend(); ++it)
        it->join();
    shared = 1;
    threads.back().join();
    return shared;
}
