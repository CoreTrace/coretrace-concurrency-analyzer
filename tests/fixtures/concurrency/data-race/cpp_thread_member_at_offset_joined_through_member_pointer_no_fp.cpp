// SPDX-License-Identifier: Apache-2.0
// The thread is a member past the start of a local, moved in and joined through a
// pointer-to-member before main writes (#206).
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

struct Holder
{
    long tag = 0;
    std::thread thread;
};

int main()
{
    Holder holder;
    holder.thread = std::thread(reader);
    call(holder.thread, &std::thread::join);
    shared = 1;
    return shared;
}
