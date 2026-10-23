// SPDX-License-Identifier: Apache-2.0
// The member function called through a pointer-to-member joins the object's thread, then starts
// other into it through a function pointer held in a local: other still runs when main writes. A
// join is not trusted from a member whose calls start a thread, even through a pointer (#195).
// The analysis reports the race against reader, which the call joined: it does not track the
// thread a call through a pointer-to-member starts (#222), and only refuses to take that join
// as known. The race being reported does not show the new thread is tracked.
// Expected: one data race on shared, reported as main against reader.
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

static void restart(std::thread& thread)
{
    thread = std::thread(other);
}

struct Worker
{
    std::thread thread;

    void finish()
    {
        thread.join();
        void (*after)(std::thread&) = restart;
        after(thread);
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
