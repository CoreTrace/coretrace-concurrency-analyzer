// SPDX-License-Identifier: Apache-2.0
// restart() resets progress through reset(). main calls restart() before starting the worker,
// which writes progress, and again while it runs: only the second call races with it (#113).
// Expected: one data race, reset's write through the second call against the worker's.
#include <pthread.h>

static int progress;

static void reset(int* value)
{
    *value = 0;
}

static void restart(void)
{
    reset(&progress);
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
    restart();
    pthread_create(&thread, NULL, worker, NULL);
    restart();
    pthread_join(thread, NULL);
    return 0;
}
