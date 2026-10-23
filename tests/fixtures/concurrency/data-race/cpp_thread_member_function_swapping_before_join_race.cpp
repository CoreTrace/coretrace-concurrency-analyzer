// SPDX-License-Identifier: Apache-2.0
// The member function swaps the thread with one running idle before joining it: the join waits
// for idle, and the reader still runs when main writes (#195).
// Expected: one data race, main against reader.
#include <thread>
#include <utility>

static int shared;
static std::thread parked;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

struct Worker
{
    std::thread thread;

    void finish()
    {
        std::swap(thread, parked);
        thread.join();
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    parked = std::thread(idle);
    Worker worker;
    worker.thread = std::thread(reader);
    call(worker, &Worker::finish);
    shared = 1;
    parked.join();
    return shared;
}
