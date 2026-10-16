// SPDX-License-Identifier: Apache-2.0
// As cpp_thread_member_joined_by_wrapper_swapping_aliased_parameter_race.cpp, with the member and
// its alias forwarded to the wrapper by another function: the join still waits for idle, and the
// reader still runs when main writes (#206).
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

static void forward(std::thread& thread, void (std::thread::*member)(), std::thread& alias)
{
    call(thread, member, alias);
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
    forward(holder.thread, &std::thread::join, holder.thread);
    shared = 1;
    parked.join();
    return shared;
}
