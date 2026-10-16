// SPDX-License-Identifier: Apache-2.0
// The holder swaps its member away only once it has been joined: what it holds after the join
// does not matter (#206).
// Expected: no diagnostic.
#include <thread>
#include <utility>

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

    void handOff(std::thread& to)
    {
        std::swap(thread, to);
    }
};

int main()
{
    Holder holder;
    holder.thread = std::thread(reader);
    call(holder.thread, &std::thread::join);
    shared = 1;
    std::thread spare;
    holder.handOff(spare);
    return shared;
}
