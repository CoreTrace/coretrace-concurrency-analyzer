// SPDX-License-Identifier: Apache-2.0
// The vector is built by moving another vector that already holds an idle thread, then a loop
// counting to 2 moves two readers in. The loop joining below 2 joins the idle thread and the first
// reader only: the second reader still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::vector<std::thread> others;
    others.push_back(std::thread([] {}));
    std::vector<std::thread> threads(std::move(others));
    for (int i = 0; i < 2; ++i)
        threads.push_back(std::thread(reader));
    for (int i = 0; i < 2; ++i)
        threads[i].join();
    shared = 1;
    threads[2].join();
    return shared;
}
