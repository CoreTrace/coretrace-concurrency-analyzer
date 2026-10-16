// SPDX-License-Identifier: Apache-2.0
// The wrapper is handed the member twice and swaps it, through its second parameter, with a thread
// running idle before calling the member on the first: the join waits for idle, and the reader
// still runs when main writes (#206).
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

static void call(std::thread& thread, void (std::thread::*member)(), std::thread& alias)
{
    std::swap(alias, parked);
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
    call(holder.thread, &std::thread::join, holder.thread);
    shared = 1;
    parked.join();
    return shared;
}
