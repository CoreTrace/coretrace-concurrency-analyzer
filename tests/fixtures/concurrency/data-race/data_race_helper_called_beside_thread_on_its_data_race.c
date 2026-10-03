// SPDX-License-Identifier: Apache-2.0
// reset() clears the integer it is handed. main calls it on other before starting the worker,
// and on progress while the worker writes progress (#113).
// Expected: one data race, reset's write against the worker's.
#include <pthread.h>

static int progress;
static int other;

static void reset(int* value)
{
    *value = 0;
}

static void* worker(void* argument)
{
    (void)argument;
    progress = 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    reset(&other);
    pthread_create(&thread, NULL, worker, NULL);
    reset(&progress);
    pthread_join(thread, NULL);
    return progress + other;
}
