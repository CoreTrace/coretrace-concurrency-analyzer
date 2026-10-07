// SPDX-License-Identifier: Apache-2.0
// std::for_each runs over an empty range, from begin() to begin(): it joins nothing, and the
// thread still runs when main writes.
// Expected: one data race, main against reader.
#include <algorithm>
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
    std::for_each(threads.begin(), threads.begin(), [](std::thread& thread) { thread.join(); });
    shared = 1;
    threads.back().join();
    return shared;
}
