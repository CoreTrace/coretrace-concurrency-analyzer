// SPDX-License-Identifier: Apache-2.0
// main writes before std::for_each joins every thread through std::mem_fn: the readers still run.
// Expected: one data race, main against reader.
#include <algorithm>
#include <functional>
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
    threads.emplace_back(std::thread(reader));
    threads.emplace_back(std::thread(reader));
    shared = 1;
    std::for_each(threads.begin(), threads.end(), std::mem_fn(&std::thread::join));
    return shared;
}
