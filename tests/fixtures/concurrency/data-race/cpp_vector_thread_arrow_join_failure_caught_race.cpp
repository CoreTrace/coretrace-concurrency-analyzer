// SPDX-License-Identifier: Apache-2.0
// Each round catches the exception it->join() may throw and goes on: a thread whose join failed
// may still run when main writes.
// Expected: one data race, main against worker.
#include <system_error>
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::vector<std::thread> threads;
    threads.push_back(std::thread(worker));
    for (auto it = threads.begin(); it != threads.end(); ++it)
    {
        try
        {
            it->join();
        }
        catch (const std::system_error&)
        {
        }
    }
    shared += 2;
    return shared;
}
