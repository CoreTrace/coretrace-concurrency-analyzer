// SPDX-License-Identifier: Apache-2.0
// A recursive helper retargets the pointer-to-member to detach at the bottom of its recursion
// before main hands it to the helper calling it on the thread: the reader still runs when main
// writes.
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

using Member = void (std::thread::*)();

static void retarget(Member& member, int depth)
{
    if (depth == 0)
    {
        member = &std::thread::detach;
        return;
    }
    retarget(member, depth - 1);
}

static void call(std::thread& thread, Member member)
{
    (thread.*member)();
}

int main()
{
    std::thread thread(reader);
    Member member = &std::thread::join;
    retarget(member, 3);
    call(thread, member);
    shared = 1;
    return shared;
}
