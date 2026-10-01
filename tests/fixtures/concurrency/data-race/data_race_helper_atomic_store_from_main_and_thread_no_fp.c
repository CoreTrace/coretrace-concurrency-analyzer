// SPDX-License-Identifier: Apache-2.0
// main and the worker thread both call store(), which writes `shared` with an atomic store: two
// atomic operations on one location never race (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

static atomic_int shared;

static void store(void)
{
    atomic_store(&shared, 42);
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
    return atomic_load(&shared);
}
