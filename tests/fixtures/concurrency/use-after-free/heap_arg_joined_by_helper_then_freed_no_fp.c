// SPDX-License-Identifier: Apache-2.0
// The memory is freed after a helper has joined the thread that uses it: the thread is done with
// it, as when the join is written in place.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

static void* worker(void* argument)
{
    int* value = (int*)argument;
    *value += 1;
    return NULL;
}

static void join_worker(pthread_t thread)
{
    pthread_join(thread, NULL);
}

void run_worker(void)
{
    int* value = malloc(sizeof *value);
    if (value == NULL)
        return;
    *value = 0;

    pthread_t thread;
    pthread_create(&thread, NULL, worker, value);
    join_worker(thread);
    free(value);
}
