// SPDX-License-Identifier: Apache-2.0
// The loop from rbegin() joins the last thread only, stopping one before rend(): the first, a
// reader, still runs when main writes.
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
    for (auto it = threads.rbegin(); it != threads.rend() - 1; ++it)
        it->join();
    shared = 1;
    threads.front().join();
    return shared;
}
