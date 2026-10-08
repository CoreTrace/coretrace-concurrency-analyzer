// SPDX-License-Identifier: Apache-2.0
// Until the vector is empty, each round joins its last thread and pops it, before main writes:
// nothing runs beside that write.
// Expected: no diagnostic.
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
    threads.emplace_back(std::thread(reader));
    while (!threads.empty())
    {
        threads.back().join();
        threads.pop_back();
    }
    shared = 1;
    return shared;
}
