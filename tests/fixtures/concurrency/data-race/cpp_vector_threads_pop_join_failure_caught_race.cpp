// SPDX-License-Identifier: Apache-2.0
// The loop catches the exception a join may throw and still pops the thread, then exits once the
// vector is empty: a thread whose join failed may still run when main writes; the program then
// exits.
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
    threads.emplace_back(std::thread(reader));
    while (!threads.empty())
    {
        try
        {
            threads.back().join();
        }
        catch (const std::system_error&)
        {
            threads.back().detach();
        }
        threads.pop_back();
    }
    shared = 1;
    return shared;
}
