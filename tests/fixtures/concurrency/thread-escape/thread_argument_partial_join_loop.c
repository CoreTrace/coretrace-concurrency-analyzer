// SPDX-License-Identifier: Apache-2.0
// The join loop stops one short of the creation loop: the last worker still reads this frame
// after the function has returned.
#include <pthread.h>
#include <stddef.h>

#define WORKER_COUNT 4

static void* worker(void* argument)
{
    volatile int id = *(int*)argument;
    (void)id;
    return NULL;
}

void run_batch(void)
{
    pthread_t threads[WORKER_COUNT];
    int ids[WORKER_COUNT];

    for (int i = 0; i < WORKER_COUNT; i++)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < WORKER_COUNT - 1; i++)
        pthread_join(threads[i], NULL);
}
