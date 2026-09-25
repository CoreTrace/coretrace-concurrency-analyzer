// SPDX-License-Identifier: Apache-2.0
// The same local, made safe by waiting through a helper: `join_worker` joins the handle it is
// given on every path, so the call waits for the thread exactly as a direct join would.
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

void run_worker(void)
{
    int local_value = 42;
    pthread_t thread;

    pthread_create(&thread, NULL, worker, &local_value);
    join_worker(thread);
}
