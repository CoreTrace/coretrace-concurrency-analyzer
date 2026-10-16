// SPDX-License-Identifier: Apache-2.0
// The holder swaps its member in the same block as the move, but before it: the handle the move
// stores is the one joined (#206).
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
    std::thread spare;
    holder.handOff(spare);
    holder.thread = std::thread(reader);
    call(holder.thread, &std::thread::join);
    shared = 1;
    return shared;
}
