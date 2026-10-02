// SPDX-License-Identifier: Apache-2.0
// Each worker reads its own slot of a local array. Two spawn loops start them over [0, 2) and
// [2, 4), and one loop joins all four before the function returns: every worker has ended, so
// the frame outlives them all (#151).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    volatile int id = *(int*)argument;
    (void)id;
    return NULL;
}

void run_workers(void)
{
    pthread_t threads[4];
    int ids[4];
    for (int i = 0; i < 2; i++)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 2; i < 4; i++)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }
    for (int i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);
}
