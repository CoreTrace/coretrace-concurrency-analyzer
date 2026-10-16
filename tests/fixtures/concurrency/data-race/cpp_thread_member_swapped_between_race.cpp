// SPDX-License-Identifier: Apache-2.0
// Between the move and the join, the member is swapped with a thread running idle: the join waits
// for idle, and the reader still runs when main writes (#206).
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
};

int main()
{
    Holder holder;
    holder.thread = std::thread(reader);
    std::thread spare(idle);
    std::swap(holder.thread, spare);
    call(holder.thread, &std::thread::join);
    shared = 1;
    spare.join();
    return shared;
}
