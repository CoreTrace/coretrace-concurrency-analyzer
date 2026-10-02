// SPDX-License-Identifier: Apache-2.0
// The temporary thread handed to emplace_back is moved into the vector, whose loop joins it before
// main writes (#162).
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
    threads.emplace_back(std::thread(worker));
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    return shared;
}
