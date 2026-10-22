// SPDX-License-Identifier: Apache-2.0
// The member function of a sub-object joins its thread, called through a pointer-to-member on the
// sub-object: its offset in the enclosing object and the thread's in it compose (#195).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

struct Inner
{
    long tag = 0;
    std::thread thread;

    void finish()
    {
        thread.join();
    }
};

struct Outer
{
    long first = 0;
    Inner inner;
};

static void call(Inner& inner, void (Inner::*member)())
{
    (inner.*member)();
}

int main()
{
    Outer outer;
    outer.inner.thread = std::thread(reader);
    call(outer.inner, &Inner::finish);
    shared = 1;
    return shared;
}
