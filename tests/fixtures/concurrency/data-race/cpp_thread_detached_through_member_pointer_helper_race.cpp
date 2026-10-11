// SPDX-License-Identifier: Apache-2.0
// A helper calls the member function a pointer-to-member names on the thread it is handed, and is
// handed &std::thread::detach: the reader still runs when main writes.
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

int main()
{
    std::thread thread(reader);
    call(thread, &std::thread::detach);
    shared = 1;
    return shared;
}
