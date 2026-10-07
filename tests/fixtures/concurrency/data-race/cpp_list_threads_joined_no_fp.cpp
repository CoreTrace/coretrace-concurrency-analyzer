// SPDX-License-Identifier: Apache-2.0
// The range-for joins every thread moved into the list before main writes: nothing runs beside
// that write.
// Expected: no diagnostic.
#include <list>
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::list<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
