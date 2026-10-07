// SPDX-License-Identifier: Apache-2.0
// The helper joining the thread catches the exception the join may throw and returns: past a
// failed join, the thread may still run when main writes.
// Expected: one data race, main against reader.
#include <system_error>
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinQuietly(std::thread& thread)
{
    try
    {
        thread.join();
    }
    catch (const std::system_error&)
    {
    }
}

int main()
{
    std::thread thread(reader);
    joinQuietly(thread);
    shared = 1;
    return shared;
}
