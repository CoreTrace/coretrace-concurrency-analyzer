// SPDX-License-Identifier: Apache-2.0
// The member function called through a pointer-to-member joins the object's thread, then calls a
// function pointer held in a local written twice: to a function starting no thread, then, on the
// path taken, to one starting other into the object. Which function the call reaches is unknown,
// and a call whose callee is unknown may start a thread: the join is not trusted (#195).
// The analysis reports the race against reader, which the call joined: it does not track the
// thread a call through a pointer-to-member starts (#222), and only refuses to take that join
// as known. The race being reported does not show the new thread is tracked.
// Expected: one data race on shared, reported as main against reader.
#include <thread>

static int shared;
static int noted;

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

static void note(std::thread& thread)
{
    (void)thread;
    ++noted;
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
        void (*after)(std::thread&) = note;
        if (noted == 0)
            after = restart;
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
    return shared + noted;
}
