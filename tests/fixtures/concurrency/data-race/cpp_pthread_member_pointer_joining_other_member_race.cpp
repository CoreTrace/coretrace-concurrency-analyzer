// SPDX-License-Identifier: Apache-2.0
// The member function called through a pointer-to-member joins the other pthread_t member: the
// reader still runs when main writes, and main joins it after (#213).
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
        pthread_join(other, nullptr);
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
