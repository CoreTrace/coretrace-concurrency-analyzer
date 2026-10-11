// SPDX-License-Identifier: Apache-2.0
// A second wrapper calls the one handing the helper &std::thread::join: the reader is joined
// before main writes.
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

static void finish(std::thread& thread)
{
    joinThread(thread);
}

int main()
{
    std::thread thread(reader);
    finish(thread);
    shared = 1;
    return shared;
}
