// SPDX-License-Identifier: Apache-2.0
// The member function joins a copy of the other member, then stores the reader's handle there:
// the join waits for idle, and the reader still runs when main writes; main joins it after (#213).
// Expected: one data race, main against reader.
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
        pthread_t joined = other;
        pthread_join(joined, nullptr);
        other = handle;
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
    pthread_join(worker.handle, nullptr);
    return shared;
}
