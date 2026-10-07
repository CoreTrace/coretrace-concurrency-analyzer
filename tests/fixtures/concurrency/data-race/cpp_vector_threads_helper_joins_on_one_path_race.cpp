// SPDX-License-Identifier: Apache-2.0
// The helper returns early, joining nothing, when told to: the thread may still run when main
// writes; the program then exits.
// Expected: one data race, main against reader.
#include <cstdlib>
#include <thread>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinAll(std::vector<std::thread>& threads, bool skip)
{
    if (skip)
        return;
    for (auto& thread : threads)
        thread.join();
}

int main(int argc, char**)
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinAll(threads, argc > 1);
    shared = 1;
    std::exit(0);
}
