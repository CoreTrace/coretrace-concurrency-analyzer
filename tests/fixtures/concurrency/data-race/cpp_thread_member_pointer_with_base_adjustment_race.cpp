// SPDX-License-Identifier: Apache-2.0
// Both holds two std::thread bases. The pointer-to-member names join through the second base, so
// calling it adjusts the object to that base: it joins the idle thread, not the reader at the
// start of the object, which still runs when main writes.
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
    static_cast<std::thread&>(static_cast<First&>(both)) = std::thread(reader);
    static_cast<std::thread&>(static_cast<Second&>(both)) = std::thread(idle);
    void (Both::*member)() = static_cast<void (Second::*)()>(&std::thread::join);
    (both.*member)();
    shared = 1;
    static_cast<First&>(both).detach();
    return shared;
}
