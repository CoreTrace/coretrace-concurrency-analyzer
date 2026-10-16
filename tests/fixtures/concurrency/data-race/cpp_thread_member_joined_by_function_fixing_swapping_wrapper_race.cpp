// SPDX-License-Identifier: Apache-2.0
// A function hands &std::thread::join to a wrapper that swaps the thread with one running idle
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

static void call(std::thread& thread, void (std::thread::*member)())
{
    std::swap(thread, parked);
    (thread.*member)();
}

static void finish(std::thread& thread)
{
    call(thread, &std::thread::join);
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
    finish(holder.thread);
    shared = 1;
    parked.join();
    return shared;
}
