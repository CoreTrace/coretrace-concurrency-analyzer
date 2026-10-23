// SPDX-License-Identifier: Apache-2.0
// As cpp_pthread_member_joined_through_member_pointer_no_fp.cpp, the pthread_t member first in its
// object (#213).
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
    pthread_t handle;
    long tag;
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
