// SPDX-License-Identifier: Apache-2.0
// Three threads are moved into the vector, but the loop joins threads[i] below 2 only: the third
// still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>
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
    for (int i = 0; i < 3; ++i)
        threads.push_back(std::thread(reader));
    for (int i = 0; i < 2; ++i)
        threads[i].join();
    shared = 1;
    threads[2].join();
    return shared;
}
