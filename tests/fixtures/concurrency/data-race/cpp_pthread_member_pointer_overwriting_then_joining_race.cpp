// SPDX-License-Identifier: Apache-2.0
// The member function overwrites the reader's handle with the other member's, then joins it: the
// join waits for idle, and the reader still runs when main writes (#213).
// Expected: one data race, main against reader, and two missing joins: the reader's, and idle's,
// which is joined through the copied handle that the analysis does not follow.
#include <pthread.h>

static int shared;

static void* reader(void*)
{
    int seen = shared;
    (void)seen;
    return nullptr;
}

static void* idle(void*)
{
    return nullptr;
}

struct Worker
{
    long tag;
    pthread_t handle;
    pthread_t other;

    void finish()
    {
        handle = other;
        pthread_join(handle, nullptr);
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    Worker worker{};
    pthread_create(&worker.handle, nullptr, reader, nullptr);
    pthread_create(&worker.other, nullptr, idle, nullptr);
    call(worker, &Worker::finish);
    shared = 1;
    return shared;
}
