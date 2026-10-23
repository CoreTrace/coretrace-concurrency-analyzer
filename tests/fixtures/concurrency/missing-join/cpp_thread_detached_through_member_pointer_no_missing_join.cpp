// SPDX-License-Identifier: Apache-2.0
// The thread is detached through a pointer-to-member naming std::thread::detach: it is neither
// abandoned joinable nor joined, and the reader still runs when main writes (#194).
// Expected: one data race, main against reader; no missing join.
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
    void (std::thread::*member)() = &std::thread::detach;
    (thread.*member)();
    shared = 1;
    return shared;
}
