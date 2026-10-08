// SPDX-License-Identifier: Apache-2.0
// main writes before the range-for joins the deque's threads: they still run.
// Expected: one data race, main against reader.
#include <deque>
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::deque<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    shared = 1;
    for (auto& thread : threads)
        thread.join();
    return shared;
}
