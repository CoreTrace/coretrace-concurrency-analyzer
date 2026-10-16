// SPDX-License-Identifier: Apache-2.0
// On one branch between the move and the join, the holder swaps its thread away: on that path the
// reader still runs when main writes (#206).
// Expected: one data race, main against reader.
#include <thread>
#include <utility>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

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
    std::thread spare(idle);
    if (shared == 0)
        holder.handOff(spare);
    call(holder.thread, &std::thread::join);
    shared = 1;
    spare.join();
    return shared;
}
