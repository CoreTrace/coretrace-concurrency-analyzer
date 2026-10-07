// SPDX-License-Identifier: Apache-2.0
// The vector is handed twice to the helper: it adds a thread through the first reference, swaps it
// out for an idle one through the second and detaches it, then joins what the first holds. The
// swapped-out thread still runs when main writes.
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
}

static void addAndJoin(std::vector<std::thread>& joined, std::vector<std::thread>& taken)
{
    add(joined);
    std::thread idle([] {});
    taken.back().swap(idle);
    idle.detach();
    for (auto& thread : joined)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    addAndJoin(threads, threads);
    shared = 1;
    return shared;
}
