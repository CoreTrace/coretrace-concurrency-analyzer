// SPDX-License-Identifier: Apache-2.0
// Each worker reads its own slot of a local array, and a second loop over the same range joins
// every handle through a helper: the frame outlives all of them.
#include <pthread.h>
#include <stddef.h>

#define WORKER_COUNT 4

static void* worker(void* argument)
{
    volatile int id = *(int*)argument;
    (void)id;
    return NULL;
}

static void join_worker(pthread_t thread)
{
    pthread_join(thread, NULL);
}

void run_workers(void)
{
    pthread_t threads[WORKER_COUNT];
    int values[WORKER_COUNT];

    for (int i = 0; i < WORKER_COUNT; i++)
    {
        values[i] = i;
        pthread_create(&threads[i], NULL, worker, &values[i]);
    }
    for (int i = 0; i < WORKER_COUNT; i++)
        join_worker(threads[i]);
}
