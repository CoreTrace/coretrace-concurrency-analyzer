// SPDX-License-Identifier: Apache-2.0
// The helper moves its thread in, then swaps it out for an idle one and detaches it: main's loop
// joins the idle thread, and the first still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void add(std::vector<std::thread>& threads)
{
    threads.emplace_back(std::thread(reader));
    std::thread idle([] {});
    threads.back().swap(idle);
    idle.detach();
}

int main()
{
    std::vector<std::thread> threads;
    add(threads);
    for (auto& thread : threads)
        thread.join();
    shared = 1;
    return shared;
}
