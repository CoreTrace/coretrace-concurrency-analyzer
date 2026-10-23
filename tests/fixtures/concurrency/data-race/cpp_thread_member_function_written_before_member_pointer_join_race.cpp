// SPDX-License-Identifier: Apache-2.0
// The member function joining the thread is called through a pointer-to-member only after main
// writes (#195).
// Expected: one data race, main against reader.
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
    shared = 1;
    call(worker, &Worker::finish);
    return shared;
}
