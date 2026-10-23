// SPDX-License-Identifier: Apache-2.0
// The thread is moved into a member past the start of its object, which a member function joins,
// called through a pointer-to-member before main writes (#195).
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
    long tag = 0;
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
    Worker worker;
    worker.thread = std::thread(reader);
    call(worker, &Worker::finish);
    shared = 1;
    return shared;
}
