// SPDX-License-Identifier: Apache-2.0
// main starts the worker thread, then calls worker() itself before the join. The entry writes
// `shared` with a single store, so the call and the thread write it at the same time (#155).
// Expected: one data race on `shared`, main's call to worker() against the worker thread.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared = 42;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    worker(NULL);
    pthread_join(thread, NULL);
    return shared;
}
