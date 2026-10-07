// SPDX-License-Identifier: Apache-2.0
// The helper takes the last thread out of the vector and detaches it before joining the others:
// that thread still runs when main writes.
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

static void joinAllButLast(std::vector<std::thread>& threads)
{
    std::thread last = std::move(threads.back());
    threads.pop_back();
    last.detach();
    for (auto& thread : threads)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinAllButLast(threads);
    shared = 1;
    return shared;
}
