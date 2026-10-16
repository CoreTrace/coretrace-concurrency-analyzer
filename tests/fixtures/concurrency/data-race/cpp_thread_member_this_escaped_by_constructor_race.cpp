// SPDX-License-Identifier: Apache-2.0
// The holder's constructor keeps its address before the move; a later call swaps the member
// through it, so the join waits for idle and the reader still runs when main writes (#206).
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

struct Holder;
static Holder* registered;
static std::thread spare;

struct Holder
{
    long tag = 0;
    std::thread thread;

    Holder()
    {
        registered = this;
    }
};

static void meddle()
{
    std::swap(registered->thread, spare);
}

int main()
{
    spare = std::thread(idle);
    Holder holder;
    holder.thread = std::thread(reader);
    meddle();
    call(holder.thread, &std::thread::join);
    shared = 1;
    spare.join();
    return shared;
}
