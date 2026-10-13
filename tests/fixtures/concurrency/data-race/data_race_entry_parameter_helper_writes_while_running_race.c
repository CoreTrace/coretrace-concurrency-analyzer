// SPDX-License-Identifier: Apache-2.0
// The helper writes while the thread it started on the handed entry runs, before joining it
// (#123).
// Expected: one data race, run against worker.
#include <pthread.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void run(void* (*entry)(void*))
{
    pthread_t thread;
    pthread_create(&thread, NULL, entry, NULL);
    shared += 1;
    pthread_join(thread, NULL);
}

int main(void)
{
    run(worker);
    return shared;
}
