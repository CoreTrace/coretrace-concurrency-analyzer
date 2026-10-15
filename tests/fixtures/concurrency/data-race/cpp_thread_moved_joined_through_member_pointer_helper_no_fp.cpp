// SPDX-License-Identifier: Apache-2.0
// The thread is moved into its variable, then a helper calls the member function a
// pointer-to-member names on it, and is handed &std::thread::join: the reader is joined before
// main writes (#196).
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
    std::thread thread;
    thread = std::thread(reader);
    call(thread, &std::thread::join);
    shared = 1;
    return shared;
}
