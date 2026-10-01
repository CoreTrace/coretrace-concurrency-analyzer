// SPDX-License-Identifier: Apache-2.0
// main starts `worker` on &a and `other`, which calls worker(&a) while both run: the call and the
// worker thread both write `a`. The thread's write is known through the object its spawn hands
// it, and only that thread makes it: `other`'s call has an access of its own, on the object it
// passes. The race is reported once (#155).
// Expected: one data race on `a`, `other`'s call to worker() against the worker thread.
#include <pthread.h>
#include <stddef.h>

static int a;

static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}

static void* other(void* argument)
{
    (void)argument;
    worker(&a);
    return NULL;
}

int main(void)
{
    pthread_t workerThread;
    pthread_t otherThread;
    pthread_create(&workerThread, NULL, worker, &a);
    pthread_create(&otherThread, NULL, other, NULL);
    pthread_join(workerThread, NULL);
    pthread_join(otherThread, NULL);
    return a;
}
