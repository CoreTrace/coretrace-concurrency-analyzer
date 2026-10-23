// SPDX-License-Identifier: Apache-2.0
// The pointer-to-member names a member of a class derived from std::thread that is neither its
// join nor its detach: the call resolves nothing, and the thread is destroyed joinable (#194).
// Expected: one data race, main against reader, and one missing join.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

struct Named : std::thread
{
    using std::thread::thread;

    void rename()
    {
        ++renamed;
    }

    int renamed = 0;
};

int main()
{
    Named thread(reader);
    void (Named::*member)() = &Named::rename;
    (thread.*member)();
    shared = 1;
    return shared;
}
