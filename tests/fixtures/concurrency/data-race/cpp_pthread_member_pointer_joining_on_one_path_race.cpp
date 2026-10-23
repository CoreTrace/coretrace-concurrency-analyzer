// SPDX-License-Identifier: Apache-2.0
// The member function joins the reader only when its tag is set, and it is not: the reader still
// runs when main writes (#213).
// Expected: one data race, main against reader, and one missing join.
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
        if (tag != 0)
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
