// SPDX-License-Identifier: Apache-2.0
// The thread kept in a local is moved into the vector on one path only. On the other, the loop
// joins nothing and the worker still runs when main writes; the program then exits (#162).
// Expected: one data race, main's write against the worker's.
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main(int argc, char**)
{
    std::vector<std::thread> threads;
    std::thread thread(worker);
    if (argc > 1)
        threads.push_back(std::move(thread));
    for (auto& joined : threads)
        joined.join();
    shared += 2;
    std::exit(0);
}
