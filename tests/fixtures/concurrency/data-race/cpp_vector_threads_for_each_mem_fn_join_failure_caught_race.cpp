// SPDX-License-Identifier: Apache-2.0
// The exception a join applied through std::mem_fn may throw is caught around std::for_each: a
// thread whose join failed may still run when main writes.
// Expected: one data race, main against reader.
#include <algorithm>
#include <functional>
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
    try
    {
        std::for_each(threads.begin(), threads.end(), std::mem_fn(&std::thread::join));
    }
    catch (const std::system_error&)
    {
    }
    shared = 1;
    return shared;
}
