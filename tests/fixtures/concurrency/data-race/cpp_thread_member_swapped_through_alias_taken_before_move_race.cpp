// SPDX-License-Identifier: Apache-2.0
// An identity function returns the holder before the move; the member is swapped through that
// alias between the move and the join, which waits for idle while the reader still runs when main
// writes (#206).
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

static Holder* same(Holder* holder)
{
    return holder;
}

int main()
{
    Holder holder;
    Holder* alias = same(&holder);
    holder.thread = std::thread(reader);
    std::thread spare(idle);
    std::swap(alias->thread, spare);
    call(holder.thread, &std::thread::join);
    shared = 1;
    spare.join();
    return shared;
}
