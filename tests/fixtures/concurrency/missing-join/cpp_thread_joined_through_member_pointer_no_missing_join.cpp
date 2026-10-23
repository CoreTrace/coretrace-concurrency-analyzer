// SPDX-License-Identifier: Apache-2.0
// The thread is joined through a pointer-to-member naming std::thread::join, before main writes
// (#194).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::thread thread(reader);
    void (std::thread::*member)() = &std::thread::join;
    (thread.*member)();
    shared = 1;
    return shared;
}
