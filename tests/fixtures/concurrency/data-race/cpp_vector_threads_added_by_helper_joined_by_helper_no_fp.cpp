// SPDX-License-Identifier: Apache-2.0
// One helper moves its threads into the vector, another joins every thread of it, before main
// writes: nothing runs beside that write.
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread(reader));
}

static void joinAll(std::vector<std::thread>& threads)
{
    for (auto& thread : threads)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    add(threads);
    add(threads);
    joinAll(threads);
    shared = 1;
    return shared;
}
