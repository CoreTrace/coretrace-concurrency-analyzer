// SPDX-License-Identifier: Apache-2.0
// As cpp_pthread_member_joined_through_member_pointer_no_fp.cpp, the member function copying the
// handle into a local before joining the copy (#213).
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
        pthread_t joined = handle;
        pthread_join(joined, nullptr);
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
