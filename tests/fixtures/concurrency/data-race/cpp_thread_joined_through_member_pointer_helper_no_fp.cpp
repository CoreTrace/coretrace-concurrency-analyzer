// SPDX-License-Identifier: Apache-2.0
// A helper calls the member function a pointer-to-member names on the thread it is handed, and is
// handed &std::thread::join: the reader is joined before main writes.
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

int main()
{
    std::thread thread(reader);
    call(thread, &std::thread::join);
    shared = 1;
    return shared;
}
