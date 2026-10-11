// SPDX-License-Identifier: Apache-2.0
// std::for_each joins every thread but the first through std::mem_fn, and the first is detached:
// its reader still runs when main writes.
// Expected: one data race, main against reader.
#include <algorithm>
#include <functional>
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
    threads.emplace_back(std::thread(reader));
    threads.front().detach();
    std::for_each(threads.begin() + 1, threads.end(), std::mem_fn(&std::thread::join));
    shared = 1;
    return shared;
}
