// SPDX-License-Identifier: Apache-2.0
// A helper hands the whole vector it is given to std::for_each with a lambda joining each thread,
// before main writes: nothing runs beside that write.
// Expected: no diagnostic.
#include <algorithm>
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinAll(std::vector<std::thread>& threads)
{
    std::for_each(threads.begin(), threads.end(), [](std::thread& thread) { thread.join(); });
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    joinAll(threads);
    shared = 1;
    return shared;
}
