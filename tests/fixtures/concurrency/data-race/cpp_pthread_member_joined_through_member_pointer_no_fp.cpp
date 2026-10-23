// SPDX-License-Identifier: Apache-2.0
// A member function joining a pthread_t member of its object, called through a pointer-to-member,
// ends the reader before main writes: the pthread_t is read from the object at the member's
// offset (#213).
// Expected: no diagnostic.
#include <pthread.h>

static int shared;

static void* reader(void*)
{
    int seen = shared;
    (void)seen;
    return nullptr;
}

struct Worker
{
    long tag;
    pthread_t handle;
    pthread_t other;

    void finish()
    {
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
    call(worker, &Worker::finish);
    shared = 1;
    return shared;
}
