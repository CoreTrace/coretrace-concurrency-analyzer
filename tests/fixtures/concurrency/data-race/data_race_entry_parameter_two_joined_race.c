// SPDX-License-Identifier: Apache-2.0
// The helper starts two threads on the entry its caller hands it and joins both before
// returning: they run at once inside the helper, so the two workers race (#123).
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
    pthread_t first;
    pthread_t second;
    pthread_create(&first, 0, entry, 0);
    pthread_create(&second, 0, entry, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
}

int main(void)
{
    run(worker);
    return shared;
}
