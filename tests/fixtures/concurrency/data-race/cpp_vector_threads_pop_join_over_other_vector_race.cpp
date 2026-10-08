// SPDX-License-Identifier: Apache-2.0
// The loop joins and pops another vector's threads until it is empty: the reader moved into this
// one still runs when main writes.
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
    while (!others.empty())
    {
        others.back().join();
        others.pop_back();
    }
    shared = 1;
    threads.back().join();
    return shared;
}
