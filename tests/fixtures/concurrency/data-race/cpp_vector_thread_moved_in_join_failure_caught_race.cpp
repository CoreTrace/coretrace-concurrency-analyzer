// SPDX-License-Identifier: Apache-2.0
// Each join sits in a try block whose handler goes on to the next round. A join that throws has not
// waited for its thread, so the worker may still run when main writes after the loop (#162).
// Expected: one data race, main's write against the worker's.
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
    for (auto& thread : threads)
    {
        try
        {
            thread.join();
        }
        catch (const std::system_error&)
        {
        }
    }
    shared += 2;
    return 0;
}
