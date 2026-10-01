// SPDX-License-Identifier: Apache-2.0
// main and the worker thread both call store(), which hands &shared to put(), and main calls it
// before the join. put() writes through its parameter with a single store, so the two calls
// write `shared` at the same time (#155).
// Expected: one data race on `shared`, main's call to store() against the worker thread's.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void put(int* slot)
{
    *slot = 42;
}

static void store(void)
{
    put(&shared);
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
