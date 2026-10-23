// SPDX-License-Identifier: Apache-2.0
// The member function called through a pointer-to-member joins the object's thread, then starts
// entry through a local pointer to pthread_create: entry still runs when main writes, and main
// joins it after. The classifier reads a direct call only, so a declared function reached
// through a pointer may start a thread, and the join is not trusted (#195).
// The analysis reports the race against reader, which the call joined: it does not track the
// thread a call through a pointer-to-member starts (#222), and only refuses to take that join
// as known. The race being reported does not show the new thread is tracked.
// Expected: one data race on shared, reported as main against reader.
#include <pthread.h>
#include <thread>

static int shared;
static pthread_t started;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void* entry(void*)
{
    int seen = shared;
    (void)seen;
    return nullptr;
}

struct Worker
{
    std::thread thread;

    void finish()
    {
        thread.join();
        int (*start)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*) = pthread_create;
        start(&started, nullptr, entry, nullptr);
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
    pthread_join(started, nullptr);
    if (worker.thread.joinable())
        worker.thread.join();
    return shared;
}
