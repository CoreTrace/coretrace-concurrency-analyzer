// SPDX-License-Identifier: Apache-2.0
// The member std::mem_fn calls is chosen at run time between join and detach: the readers may
// still run when main writes.
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

int main(int argc, char**)
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    void (std::thread::*member)() = argc > 1 ? &std::thread::detach : &std::thread::join;
    std::for_each(threads.begin(), threads.end(), std::mem_fn(member));
    shared = 1;
    return shared;
}
