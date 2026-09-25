// SPDX-License-Identifier: Apache-2.0
// A helper that joins only when asked to: on the path where it does not, the frame is gone while
// the thread still reads it, so the call cannot stand for a join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    int* value = (int*)argument;
    *value += 1;
    return NULL;
}

static void maybe_join(pthread_t thread, int wait)
{
    if (wait)
        pthread_join(thread, NULL);
}

void run_worker(int wait)
{
    int local_value = 42;
    pthread_t thread;

    pthread_create(&thread, NULL, worker, &local_value);
    maybe_join(thread, wait);
}
