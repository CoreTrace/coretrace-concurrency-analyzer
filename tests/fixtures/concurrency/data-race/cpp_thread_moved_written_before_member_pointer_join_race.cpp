// SPDX-License-Identifier: Apache-2.0
// The thread is moved into its variable and joined through a pointer-to-member only after main
// writes: the reader still runs when main writes (#196).
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
    std::thread thread;
    thread = std::thread(reader);
    shared = 1;
    call(thread, &std::thread::join);
    return shared;
}
