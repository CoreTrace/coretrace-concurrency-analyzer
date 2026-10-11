// SPDX-License-Identifier: Apache-2.0
// A wrapper hands the helper calling a pointer-to-member &std::thread::detach itself: the reader
// still runs when main writes.
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void call(std::thread& thread, void (std::thread::*member)())
{
    (thread.*member)();
}

static void detachThread(std::thread& thread)
{
    call(thread, &std::thread::detach);
}

int main()
{
    std::thread thread(reader);
    detachThread(thread);
    shared = 1;
    return shared;
}
