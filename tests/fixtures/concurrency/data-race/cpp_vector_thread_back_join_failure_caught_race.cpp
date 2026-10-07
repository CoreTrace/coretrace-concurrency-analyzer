// SPDX-License-Identifier: Apache-2.0
// The exception back().join() may throw is caught, and main goes on: a thread whose join failed
// may still run when main writes.
// Expected: one data race, main against reader.
#include <system_error>
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
    try
    {
        threads.back().join();
    }
    catch (const std::system_error&)
    {
    }
    shared = 1;
    return shared;
}
