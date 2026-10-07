// SPDX-License-Identifier: Apache-2.0
// main takes the thread out of the vector and detaches it before handing the vector to the helper
// joining its threads: the thread still runs when main writes.
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

static void joinAll(std::vector<std::thread>& threads)
{
    for (auto& thread : threads)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    std::thread taken = std::move(threads.back());
    threads.pop_back();
    taken.detach();
    joinAll(threads);
    shared = 1;
    return shared;
}
