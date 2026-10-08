// SPDX-License-Identifier: Apache-2.0
// The thread moved into the list is swapped out for an idle one and detached before the range-for
// joins the list: the reader still runs when main writes.
// Expected: one data race, main against reader.
#include <list>
#include <thread>

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
    std::thread idle([] {});
    threads.back().swap(idle);
    idle.detach();
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
