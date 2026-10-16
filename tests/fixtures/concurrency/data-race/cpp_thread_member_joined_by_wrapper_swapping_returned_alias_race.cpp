// SPDX-License-Identifier: Apache-2.0
// The wrapper swaps the thread it is handed, reached through what an identity function returns,
// before calling the member: the join waits for idle, and the reader still runs when main writes
// (#206).
// Expected: one data race, main against reader.
#include <thread>
#include <utility>

static int shared;
static std::thread parked;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

static std::thread& same(std::thread& thread)
{
    return thread;
}

static void call(std::thread& thread, void (std::thread::*member)())
{
    std::swap(same(thread), parked);
    (thread.*member)();
}

struct Holder
{
    long tag = 0;
    std::thread thread;
};

int main()
{
    parked = std::thread(idle);
    Holder holder;
    holder.thread = std::thread(reader);
    call(holder.thread, &std::thread::join);
    shared = 1;
    parked.join();
    return shared;
}
