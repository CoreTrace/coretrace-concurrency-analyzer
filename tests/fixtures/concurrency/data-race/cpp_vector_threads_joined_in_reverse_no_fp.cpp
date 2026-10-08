// SPDX-License-Identifier: Apache-2.0
// A loop from rbegin() to rend() joins every thread of the vector before main writes: nothing
// runs beside that write.
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
    for (auto it = threads.rbegin(); it != threads.rend(); ++it)
        it->join();
    shared = 1;
    return shared;
}
