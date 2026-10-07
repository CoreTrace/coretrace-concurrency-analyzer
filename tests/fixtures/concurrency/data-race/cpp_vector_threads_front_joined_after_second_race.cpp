// SPDX-License-Identifier: Apache-2.0
// Two threads are moved into the vector and front() joins the first, which reads nothing, only:
// the second still runs when main writes.
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
    threads.push_back(std::thread([] {}));
    threads.push_back(std::thread(reader));
    threads.front().join();
    shared = 1;
    threads.back().join();
    return shared;
}
