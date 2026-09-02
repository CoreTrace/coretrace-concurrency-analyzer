// SPDX-License-Identifier: Apache-2.0
// The worker is handed the address of a local, and the creator detaches instead of waiting.
// The frame is reclaimed as soon as start_worker returns, while the thread still reads it.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    int* value = (int*)argument;
    return (void*)(long)(*value);
}

void start_worker(void)
{
    int local_value = 42;
    pthread_t helper;

    pthread_create(&helper, NULL, worker, &local_value);
    pthread_detach(helper);
}
