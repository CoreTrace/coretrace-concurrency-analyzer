// SPDX-License-Identifier: Apache-2.0
// The helper joining every thread of the vector is also handed a constant count: a constant is no
// alias of the vector, and the reader is joined before main writes.
// Expected: no diagnostic.
#include <cstddef>
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinAll(std::vector<std::thread>& threads, std::size_t expected)
{
    (void)expected;
    for (auto& thread : threads)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinAll(threads, 1);
    shared = 1;
    return shared;
}
