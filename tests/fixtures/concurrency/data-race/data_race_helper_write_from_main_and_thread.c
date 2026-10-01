// SPDX-License-Identifier: Apache-2.0
// main and the worker thread both call store(), which writes `shared` with a single store, and
// main calls it before the join. The two calls write `shared` at the same time (#155).
// Expected: one data race on `shared`, main's call to store() against the worker thread's.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void store(void)
{
    shared = 42;
}

static void* worker(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    store();
    pthread_join(thread, NULL);
    return shared;
}
