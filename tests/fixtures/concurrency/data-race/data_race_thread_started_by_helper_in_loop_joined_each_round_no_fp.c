// SPDX-License-Identifier: Apache-2.0
// A helper starts the thread in each round of a loop, into the same handle, and the round joins
// it before the next round starts another. main writes shared after the join: the thread has
// ended, and the next one is not started yet (#126).
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
    for (int i = 0; i < 2; ++i)
    {
        pthread_t thread;
        start(&thread);
        pthread_join(thread, NULL);
        shared = 0;
    }
    return 0;
}
