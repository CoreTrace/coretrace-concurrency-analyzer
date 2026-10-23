// SPDX-License-Identifier: Apache-2.0
// The member function hands the thread its object holds to a helper joining it: the offset of the
// member and the helper's join compose (#195).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void joinIt(std::thread& thread)
{
    thread.join();
}

struct Worker
{
    long tag = 0;
    std::thread thread;

    void finish()
    {
        joinIt(thread);
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
