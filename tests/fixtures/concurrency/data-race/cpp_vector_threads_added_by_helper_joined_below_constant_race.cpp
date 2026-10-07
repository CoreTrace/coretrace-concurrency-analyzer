// SPDX-License-Identifier: Apache-2.0
// The loop joins the vector below a constant, one, while a helper moved two threads in: the second
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

static void addTwo(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread([] {}));
    threads.emplace_back(std::thread(reader));
}

int main()
{
    std::vector<std::thread> threads;
    addTwo(threads);
    for (int index = 0; index < 1; ++index)
        threads[index].join();
    shared = 1;
    threads.back().join();
    return shared;
}
