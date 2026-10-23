// SPDX-License-Identifier: Apache-2.0
// As cpp_thread_detached_through_member_pointer_no_missing_join.cpp, the detach made on one
// branch only: the other destroys the thread joinable (#194).
// Expected: one data race, main against reader, and one missing join.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main(int argc, char**)
{
    std::thread thread(reader);
    void (std::thread::*member)() = &std::thread::detach;
    if (argc > 1)
        (thread.*member)();
    shared = 1;
    return shared;
}
