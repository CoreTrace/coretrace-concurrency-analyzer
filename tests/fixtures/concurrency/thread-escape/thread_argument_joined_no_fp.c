// SPDX-License-Identifier: Apache-2.0
// The same local, made safe by waiting: the join stands between the creation and the only way
// out, so the frame outlives the thread.
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
    pthread_join(helper, NULL);
}
