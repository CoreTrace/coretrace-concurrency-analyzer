// SPDX-License-Identifier: Apache-2.0
// The vector is handed to the helper twice, the second time as the reference a function returns:
// through it, the helper swaps the reader out for an idle thread and detaches it, then joins what
// the first parameter holds. The reader still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static std::vector<std::thread>& identity(std::vector<std::thread>& threads)
{
    return threads;
}

static void joinFirst(std::vector<std::thread>& joined, std::vector<std::thread>& taken)
{
    std::thread idle([] {});
    taken.back().swap(idle);
    idle.detach();
    for (auto& thread : joined)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinFirst(threads, identity(threads));
    shared = 1;
    return shared;
}
