// SPDX-License-Identifier: Apache-2.0
// The pointer-to-member detaches another thread than the reader's, which is destroyed joinable:
// a detach ends only the thread of the object it is called on (#194).
// Expected: one data race, main against reader, and one missing join, for the reader's thread.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

int main()
{
    std::thread thread(reader);
    std::thread other(idle);
    void (std::thread::*member)() = &std::thread::detach;
    (other.*member)();
    shared = 1;
    return shared;
}
