// SPDX-License-Identifier: Apache-2.0
// A loop below the deque's size joins each thread by index before main writes: nothing runs
// beside that write.
// Expected: no diagnostic.
#include <cstddef>
#include <deque>
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::deque<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    for (std::size_t index = 0; index < threads.size(); ++index)
        threads[index].join();
    shared = 1;
    return shared;
}
