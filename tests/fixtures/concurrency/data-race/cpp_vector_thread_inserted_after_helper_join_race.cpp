// SPDX-License-Identifier: Apache-2.0
// A thread moved into the vector after the helper joined it is not joined: it still runs when
// main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinAll(std::vector<std::thread>& threads)
{
    for (auto& thread : threads)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread([] {}));
    joinAll(threads);
    threads.emplace_back(std::thread(reader));
    shared = 1;
    threads.back().join();
    return shared;
}
