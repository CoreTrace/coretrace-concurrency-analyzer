// SPDX-License-Identifier: Apache-2.0
// The helper catches the exception a join may throw and goes on: a thread whose join failed may
// still run when main writes.
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

static void joinAll(std::vector<std::thread>& threads)
{
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
}

int main()
{
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(reader));
    joinAll(threads);
    shared = 1;
    return shared;
}
