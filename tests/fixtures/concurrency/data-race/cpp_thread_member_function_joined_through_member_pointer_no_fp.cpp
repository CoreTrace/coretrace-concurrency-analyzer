// SPDX-License-Identifier: Apache-2.0
// A member function joining the thread its object holds, called through a pointer-to-member, ends
// that thread before main writes (#195).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

struct Worker
{
    std::thread thread;

    void finish()
    {
        thread.join();
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    Worker worker{std::thread(reader)};
    call(worker, &Worker::finish);
    shared = 1;
    return shared;
}
