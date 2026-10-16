// SPDX-License-Identifier: Apache-2.0
// The thread moved into the member of a local is joined through a pointer-to-member only after
// main writes: the reader still runs (#206).
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

struct Holder
{
    long tag = 0;
    std::thread thread;
};

int main()
{
    Holder holder;
    holder.thread = std::thread(reader);
    shared = 1;
    call(holder.thread, &std::thread::join);
    return shared;
}
