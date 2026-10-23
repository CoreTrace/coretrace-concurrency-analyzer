// SPDX-License-Identifier: Apache-2.0
// The enclosing object's member function joins through the member function of its sub-object,
// called through a pointer-to-member on the enclosing object: the offsets compose through both
// member functions (#195).
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

    void finish()
    {
        inner.finish();
    }
};

static void call(Outer& outer, void (Outer::*member)())
{
    (outer.*member)();
}

int main()
{
    Outer outer;
    outer.inner.thread = std::thread(reader);
    call(outer, &Outer::finish);
    shared = 1;
    return shared;
}
