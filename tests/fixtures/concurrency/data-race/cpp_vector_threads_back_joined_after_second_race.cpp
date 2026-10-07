// SPDX-License-Identifier: Apache-2.0
// Two threads are moved into the vector and back() joins the second, which reads nothing, only:
// the first still runs when main writes.
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
    threads.push_back(std::thread(reader));
    threads.push_back(std::thread([] {}));
    threads.back().join();
    shared = 1;
    threads.front().join();
    return shared;
}
