// SPDX-License-Identifier: Apache-2.0
// std::for_each joins the first thread of the vector only, its range ending one past it: the
// second thread still runs when main writes.
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
    threads.emplace_back(std::thread([] {}));
    threads.emplace_back(std::thread(reader));
    std::for_each(threads.begin(), threads.begin() + 1, [](std::thread& thread) { thread.join(); });
    shared = 1;
    threads.back().join();
    return shared;
}
