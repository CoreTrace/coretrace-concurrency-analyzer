// SPDX-License-Identifier: Apache-2.0
// std::for_each joins the threads of another vector: the thread moved into this one still runs
// when main writes.
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
    std::vector<std::thread> others;
    threads.emplace_back(std::thread(reader));
    others.emplace_back(std::thread([] {}));
    std::for_each(others.begin(), others.end(), [](std::thread& thread) { thread.join(); });
    shared = 1;
    threads.back().join();
    return shared;
}
