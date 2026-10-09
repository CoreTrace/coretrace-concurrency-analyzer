// SPDX-License-Identifier: Apache-2.0
// The helper is handed the same pointer-to-member twice: through the second reference it names
// detach, then calls the first one, which now names detach too. The reader still runs when main
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

static void call(std::thread& thread, const Member& member, Member& alias)
{
    alias = &std::thread::detach;
    (thread.*member)();
}

int main()
{
    std::thread thread(reader);
    Member member = &std::thread::join;
    call(thread, member, member);
    shared = 1;
    return shared;
}
