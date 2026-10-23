// SPDX-License-Identifier: Apache-2.0
// The member function joins the thread only when a flag is set, which it is not: the reader may
// still run when main writes (#195).
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static bool ready;

struct Worker
{
    std::thread thread;

    void finish()
    {
        if (ready)
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
    if (worker.thread.joinable())
        worker.thread.join();
    return shared;
}
