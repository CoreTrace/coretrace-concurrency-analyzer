// SPDX-License-Identifier: Apache-2.0
// A wrapper hands the helper calling a pointer-to-member &std::thread::join itself: the reader is
// joined before main writes.
// Expected: no diagnostic.
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

static void joinThread(std::thread& thread)
{
    call(thread, &std::thread::join);
}

int main()
{
    std::thread thread(reader);
    joinThread(thread);
    shared = 1;
    return shared;
}
