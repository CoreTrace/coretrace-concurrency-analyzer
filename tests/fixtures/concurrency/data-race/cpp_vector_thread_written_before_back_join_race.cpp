// SPDX-License-Identifier: Apache-2.0
// main writes before joining the vector's only thread through back(): that thread still runs.
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
    shared = 1;
    threads.back().join();
    return shared;
}
