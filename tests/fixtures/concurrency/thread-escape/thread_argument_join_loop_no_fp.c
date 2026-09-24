// SPDX-License-Identifier: Apache-2.0
// Every worker gets a pointer into this frame, and a second loop over the same range joins every
// one of them before the function returns: no join dominates the return, yet the frame outlives
// all the threads.
#include <pthread.h>
#include <stddef.h>

#define WORKER_COUNT 4

static void* worker(void* argument)
{
    volatile int id = *(int*)argument;
    (void)id;
    return NULL;
}

int main(void)
{
    pthread_t threads[WORKER_COUNT];
    int ids[WORKER_COUNT];

    for (int i = 0; i < WORKER_COUNT; i++)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < WORKER_COUNT; i++)
        pthread_join(threads[i], NULL);

    return 0;
}
