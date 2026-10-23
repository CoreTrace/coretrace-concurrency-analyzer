// SPDX-License-Identifier: Apache-2.0
// The member function called through a pointer-to-member joins the object's thread, then starts
// reader again into it: the new reader still runs when main writes (#195).
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void other()
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
        thread = std::thread(reader);
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
    worker.thread.join();
    return shared;
}
