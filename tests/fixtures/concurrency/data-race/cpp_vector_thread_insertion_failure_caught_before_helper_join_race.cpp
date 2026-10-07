// SPDX-License-Identifier: Apache-2.0
// push_back sits in a try block whose handler goes on. A push_back that throws has not moved the
// thread, so the helper joins nothing of it, and the reader still runs when main writes; the
// program then exits.
// Expected: one data race, main against reader.
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinAll(std::vector<std::thread>& threads)
{
    for (auto& thread : threads)
        thread.join();
}

int main()
{
    std::vector<std::thread> threads;
    std::thread thread(reader);
    try
    {
        threads.push_back(std::move(thread));
    }
    catch (...)
    {
    }
    joinAll(threads);
    shared = 1;
    std::exit(0);
}
