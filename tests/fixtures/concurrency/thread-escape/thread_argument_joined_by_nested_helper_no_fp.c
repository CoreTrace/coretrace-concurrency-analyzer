// SPDX-License-Identifier: Apache-2.0
// The join is two calls away: `stop_worker` hands the handle to `join_worker`, which joins it.
// A helper that calls a joining helper on every path joins just as well.
#include <pthread.h>
#include <stddef.h>

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

static void stop_worker(pthread_t thread)
{
    join_worker(thread);
}

void run_worker(void)
{
    int local_value = 42;
    pthread_t thread;

    pthread_create(&thread, NULL, worker, &local_value);
    stop_worker(thread);
}
