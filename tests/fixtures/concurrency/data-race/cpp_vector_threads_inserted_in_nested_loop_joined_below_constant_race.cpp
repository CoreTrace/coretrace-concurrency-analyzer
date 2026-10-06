// SPDX-License-Identifier: Apache-2.0
// An inner loop counting to 2 moves a thread into the vector each round, and an outer loop runs it
// twice: the vector holds four threads, and the loop joining below 2 leaves two running when main
// writes.
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

int main()
{
    std::vector<std::thread> threads;
    for (int round = 0; round < 2; ++round)
        for (int i = 0; i < 2; ++i)
            threads.push_back(std::thread(reader));
    for (int i = 0; i < 2; ++i)
        threads[i].join();
    shared = 1;
    for (int i = 2; i < 4; ++i)
        threads[i].join();
    return shared;
}
