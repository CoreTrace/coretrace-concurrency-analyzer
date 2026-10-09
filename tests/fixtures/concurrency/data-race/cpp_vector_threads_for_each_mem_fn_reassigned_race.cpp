// SPDX-License-Identifier: Apache-2.0
// The std::mem_fn object first names join, then is reassigned to name detach before std::for_each
// applies it: the readers still run when main writes.
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
    auto applied = std::mem_fn(&std::thread::join);
    applied = std::mem_fn(&std::thread::detach);
    std::for_each(threads.begin(), threads.end(), applied);
    shared = 1;
    return shared;
}
