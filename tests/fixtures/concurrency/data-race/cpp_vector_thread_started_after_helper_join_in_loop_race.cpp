// SPDX-License-Identifier: Apache-2.0
// Each round hands the vector to the helper, then moves a new thread into it: the last round's
// thread is joined by no helper call before main writes.
// Expected: one data race, main against reader.
#include <thread>
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
    int round = 0;
    do
    {
        joinAll(threads);
        threads.clear();
        threads.emplace_back(std::thread(reader));
    } while (++round < 2);
    shared = 1;
    joinAll(threads);
    return shared;
}
