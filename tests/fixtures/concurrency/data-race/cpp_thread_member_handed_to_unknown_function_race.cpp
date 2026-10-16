// SPDX-License-Identifier: Apache-2.0
// Between the move and the join, the holder is handed to a function the unit does not define,
// which may swap its thread away (#206).
// Expected: one data race, main against reader.
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
    long tag = 0;
    std::thread thread;
};

void inspect(Holder& holder);

int main()
{
    Holder holder;
    holder.thread = std::thread(reader);
    inspect(holder);
    call(holder.thread, &std::thread::join);
    shared = 1;
    return shared;
}
