// SPDX-License-Identifier: Apache-2.0
// A helper starts the thread; main joins each thread before calling the helper again, so the two
// threads never run at once (#126).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void start(pthread_t* thread)
{
    pthread_create(thread, NULL, worker, NULL);
}

int main(void)
{
    pthread_t first;
    pthread_t second;
    start(&first);
    pthread_join(first, NULL);
    start(&second);
    pthread_join(second, NULL);
    return 0;
}
