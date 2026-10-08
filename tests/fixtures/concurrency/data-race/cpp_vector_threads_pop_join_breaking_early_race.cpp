// SPDX-License-Identifier: Apache-2.0
// The loop joins and pops the last thread, then breaks out before the vector is empty: the first
// thread, a reader, still runs when main writes.
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
    threads.emplace_back(std::thread([] {}));
    while (!threads.empty())
    {
        threads.back().join();
        threads.pop_back();
        break;
    }
    shared = 1;
    threads.back().join();
    return shared;
}
