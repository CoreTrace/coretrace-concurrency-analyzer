// SPDX-License-Identifier: Apache-2.0
// A thread started into a temporary and moved into the vector by push_back is joined by the loop
// over the vector before main writes: nothing runs beside that write (#162).
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
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    return shared;
}
