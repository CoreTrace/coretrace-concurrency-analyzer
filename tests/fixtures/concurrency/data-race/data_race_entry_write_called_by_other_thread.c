// SPDX-License-Identifier: Apache-2.0
// main starts `worker` and `other`, which calls worker() itself while both run. The entry writes
// `shared` with a single store, so the call and the worker thread write it at the same time
// (#155).
// Expected: one data race on `shared`, `other`'s call to worker() against the worker thread.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared = 42;
    return NULL;
}

static void* other(void* argument)
{
    (void)argument;
    worker(NULL);
    return NULL;
}

int main(void)
{
    pthread_t workerThread;
    pthread_t otherThread;
    pthread_create(&workerThread, NULL, worker, NULL);
    pthread_create(&otherThread, NULL, other, NULL);
    pthread_join(workerThread, NULL);
    pthread_join(otherThread, NULL);
    return shared;
}
