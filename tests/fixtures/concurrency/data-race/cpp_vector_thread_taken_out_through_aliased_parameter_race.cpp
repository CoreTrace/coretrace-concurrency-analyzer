// SPDX-License-Identifier: Apache-2.0
// The vector is handed to the helper twice: through its second parameter, the helper takes the
// thread out and detaches it, then joins what the first one holds. The thread still runs when
// main writes.
// Expected: one data race, main against reader.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinFirst(std::vector<std::thread>& joined, std::vector<std::thread>& taken)
{
    std::thread last = std::move(taken.back());
    taken.pop_back();
    last.detach();
    for (auto& thread : joined)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinFirst(threads, threads);
    shared = 1;
    return shared;
}
