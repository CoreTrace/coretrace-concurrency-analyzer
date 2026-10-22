// SPDX-License-Identifier: Apache-2.0
// The member function joins the slot of its thread array a member selects, which holds the idle
// thread: the reader still runs when main writes (#195).
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
    std::thread threads[2];
    int pick = 1;

    void finish()
    {
        threads[pick].join();
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    Worker worker;
    worker.threads[0] = std::thread(reader);
    worker.threads[1] = std::thread(idle);
    call(worker, &Worker::finish);
    shared = 1;
    worker.threads[0].join();
    return shared;
}
