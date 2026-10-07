// SPDX-License-Identifier: Apache-2.0
// The thread moved into the list is moved out and detached, and the list popped, before the
// range-for joins what is left: the reader still runs when main writes.
// Expected: one data race, main against reader.
#include <list>
#include <thread>
#include <utility>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::list<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    std::thread taken = std::move(threads.back());
    threads.pop_back();
    taken.detach();
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
