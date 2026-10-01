// SPDX-License-Identifier: Apache-2.0
// main starts two worker threads and calls worker() itself before joining them. The entry writes
// `shared` with a single store: the two threads race with each other, and each with main's call.
// All of them are the same write to the same place, reported once (#155).
// Expected: one data race on `shared`.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared = 42;
    return NULL;
}

int main(void)
{
    pthread_t first;
    pthread_t second;
    pthread_create(&first, NULL, worker, NULL);
    pthread_create(&second, NULL, worker, NULL);
    worker(NULL);
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return shared;
}
