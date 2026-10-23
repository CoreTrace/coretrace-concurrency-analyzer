// SPDX-License-Identifier: Apache-2.0
// The pointer-to-member naming the member function joining the thread is forwarded to the wrapper
// calling it by another function (#195).
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

static void forward(Worker& worker, void (Worker::*member)())
{
    call(worker, member);
}

int main()
{
    Worker worker;
    worker.thread = std::thread(reader);
    forward(worker, &Worker::finish);
    shared = 1;
    return shared;
}
