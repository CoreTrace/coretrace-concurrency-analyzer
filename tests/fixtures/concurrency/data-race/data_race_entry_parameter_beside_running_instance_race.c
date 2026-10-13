// SPDX-License-Identifier: Apache-2.0
// Main starts a worker, then hands the same entry to a helper that starts and joins another
// before returning: the helper's thread runs beside main's, so the two workers race (#123).
// Expected: one data race, worker against worker.
#include <pthread.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return 0;
}

static void run(void* (*entry)(void*))
{
    pthread_t thread;
    pthread_create(&thread, 0, entry, 0);
    pthread_join(thread, 0);
}

int main(void)
{
    pthread_t first;
    pthread_create(&first, 0, worker, 0);
    run(worker);
    pthread_join(first, 0);
    return shared;
}
