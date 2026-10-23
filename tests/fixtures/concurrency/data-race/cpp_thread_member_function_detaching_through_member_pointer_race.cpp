// SPDX-License-Identifier: Apache-2.0
// The member function called through the pointer-to-member detaches the thread instead of joining
// it: the reader still runs when main writes (#195).
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
    std::thread thread;

    void finish()
    {
        thread.detach();
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
