// SPDX-License-Identifier: Apache-2.0
// The thread is moved into the member of a local and joined through a pointer-to-member before
// main writes. The holder's constructor and destructor are handed the local, but neither runs
// between the move and the join (#206).
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
