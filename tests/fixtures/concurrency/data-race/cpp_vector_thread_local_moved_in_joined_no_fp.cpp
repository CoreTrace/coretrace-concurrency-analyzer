// SPDX-License-Identifier: Apache-2.0
// A thread kept in a local is moved into the vector with std::move, and the loop over the vector
// joins it before main writes (#162).
// Expected: no diagnostic.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::vector<std::thread> threads;
    std::thread thread(worker);
    threads.push_back(std::move(thread));
    for (auto& joined : threads)
        joined.join();
    shared += 2;
    return shared;
}
