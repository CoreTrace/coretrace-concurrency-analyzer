// SPDX-License-Identifier: Apache-2.0
// The loop joins the vector by index below a size read once before it, and moves a new thread in
// at each round: the threads it appends lie past that size, and still run when main writes.
// Expected: one data race, main against reader.
#include <cstddef>
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
    threads.push_back(std::thread(reader));
    const std::size_t count = threads.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        threads[i].join();
        threads.push_back(std::thread(reader));
    }
    shared = 1;
    for (std::size_t i = count; i < threads.size(); ++i)
        threads[i].join();
    return shared;
}
