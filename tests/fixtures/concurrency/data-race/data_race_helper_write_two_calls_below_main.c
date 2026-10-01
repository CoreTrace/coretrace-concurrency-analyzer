// SPDX-License-Identifier: Apache-2.0
// main reaches store() through update(), and the worker thread calls it directly, both before
// the join. store() writes `shared` with a single store, so the two calls write it at the same
// time (#155).
// Expected: one data race on `shared`, main's call through update() against the worker thread's.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void store(void)
{
    shared = 42;
}

static void update(void)
{
    store();
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
    update();
    pthread_join(thread, NULL);
    return shared;
}
