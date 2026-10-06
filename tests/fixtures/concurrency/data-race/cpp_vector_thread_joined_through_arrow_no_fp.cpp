// SPDX-License-Identifier: Apache-2.0
// A thread moved into the vector is joined by an iterator loop calling it->join() over the whole
// vector before main writes: nothing runs beside that write.
// Expected: no diagnostic.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::vector<std::thread> threads;
    threads.push_back(std::thread(worker));
    for (auto it = threads.begin(); it != threads.end(); ++it)
        it->join();
    shared += 2;
    return shared;
}
