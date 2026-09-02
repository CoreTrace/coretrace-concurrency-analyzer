// SPDX-License-Identifier: Apache-2.0
// The argument outlives every frame, so detaching is fine.
#include <pthread.h>
#include <stddef.h>

static int shared_value = 42;

static void* worker(void* argument)
{
    int* value = (int*)argument;
    return (void*)(long)(*value);
}

void start_worker(void)
{
    pthread_t helper;

    pthread_create(&helper, NULL, worker, &shared_value);
    pthread_detach(helper);
}
