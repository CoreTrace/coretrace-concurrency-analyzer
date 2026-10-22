// SPDX-License-Identifier: Apache-2.0
// The member function joins the object's second thread, running idle, created before the reader's
// first, which is the last store into the object before the join: the reader still runs when main
// writes (#195).
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

struct Worker
{
    std::thread first;
    std::thread second;

    void finish()
    {
        second.join();
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    Worker worker;
    worker.second = std::thread(idle);
    worker.first = std::thread(reader);
    call(worker, &Worker::finish);
    shared = 1;
    worker.first.join();
    return shared;
}
