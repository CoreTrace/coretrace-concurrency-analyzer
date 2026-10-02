// SPDX-License-Identifier: Apache-2.0
// push_back sits in a try block whose handler goes on. A push_back that throws has not moved the
// thread, so the loop may join nothing of it, and the worker still runs when main writes; the
// program then exits (#162).
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

int main()
{
    std::vector<std::thread> threads;
    std::thread thread(worker);
    try
    {
        threads.push_back(std::move(thread));
    }
    catch (...)
    {
    }
    for (auto& joined : threads)
        joined.join();
    shared += 2;
    std::exit(0);
}
