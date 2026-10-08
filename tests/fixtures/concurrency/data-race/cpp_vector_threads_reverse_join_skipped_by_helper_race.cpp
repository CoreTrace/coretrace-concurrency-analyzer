// SPDX-License-Identifier: Apache-2.0
// Each round of the loop from rbegin() to rend() hands its iterator to a helper that steps it once
// more before the join: the loop joins the idle thread only and skips the reader, which still runs
// when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

using Iterator = std::vector<std::thread>::reverse_iterator;

static void skip(int, Iterator& it)
{
    ++it;
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(idle));
    threads.emplace_back(std::thread(reader));
    for (auto it = threads.rbegin(); it != threads.rend(); ++it)
    {
        skip(0, it);
        it->join();
    }
    shared = 1;
    threads[1].join();
    return shared;
}
