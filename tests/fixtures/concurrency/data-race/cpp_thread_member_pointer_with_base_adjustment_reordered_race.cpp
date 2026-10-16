// SPDX-License-Identifier: Apache-2.0
// As cpp_thread_member_pointer_with_base_adjustment_race.cpp, with the second base's thread moved
// in first: nothing then writes the object between the reader's move and the join, and only the
// pair's adjustment tells that the join reaches the second base (#206).
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

struct First : std::thread
{
};

struct Second : std::thread
{
};

struct Both : First, Second
{
};

int main()
{
    Both both;
    static_cast<std::thread&>(static_cast<Second&>(both)) = std::thread(idle);
    static_cast<std::thread&>(static_cast<First&>(both)) = std::thread(reader);
    void (Both::*member)() = static_cast<void (Second::*)()>(&std::thread::join);
    (both.*member)();
    shared = 1;
    static_cast<First&>(both).detach();
    return shared;
}
