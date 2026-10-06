// SPDX-License-Identifier: Apache-2.0
// Each round of the loop joining below 3 catches the exception join() may throw and goes on: a
// thread whose join failed may still run when main writes.
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
    for (int i = 0; i < 3; ++i)
        threads.push_back(std::thread(reader));
    for (int i = 0; i < 3; ++i)
    {
        try
        {
            threads[i].join();
        }
        catch (const std::system_error&)
        {
        }
    }
    shared = 1;
    return shared;
}
