// SPDX-License-Identifier: Apache-2.0
// Until the list is empty, each round joins its last thread and pops it, before main writes:
// nothing runs beside that write.
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
    while (!threads.empty())
    {
        threads.back().join();
        threads.pop_back();
    }
    shared = 1;
    return shared;
}
