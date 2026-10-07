// SPDX-License-Identifier: Apache-2.0
// A helper joins every thread of the vector handed to it before main writes: nothing runs beside
// that write.
// Expected: no diagnostic.
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
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    joinAll(threads);
    shared = 1;
    return shared;
}
