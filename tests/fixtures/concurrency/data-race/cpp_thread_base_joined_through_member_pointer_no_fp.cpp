// SPDX-License-Identifier: Apache-2.0
// The thread is the std::thread base of a local, moved in and joined through a pointer-to-member
// before main writes (#206).
// Expected: no diagnostic.
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

struct Holder : std::thread
{
};

int main()
{
    Holder holder;
    static_cast<std::thread&>(holder) = std::thread(reader);
    call(holder, &std::thread::join);
    shared = 1;
    return shared;
}
