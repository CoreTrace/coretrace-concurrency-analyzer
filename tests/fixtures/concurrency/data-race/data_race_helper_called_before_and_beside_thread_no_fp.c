// SPDX-License-Identifier: Apache-2.0
// reset() clears the integer it is handed. main calls it on progress before starting the worker,
// which then writes progress, and on other while the worker runs: neither call reaches what the
// worker writes while it runs (#113).
// Expected: no diagnostic.
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
    reset(&progress);
    pthread_create(&thread, NULL, worker, NULL);
    reset(&other);
    pthread_join(thread, NULL);
    return progress + other;
}
