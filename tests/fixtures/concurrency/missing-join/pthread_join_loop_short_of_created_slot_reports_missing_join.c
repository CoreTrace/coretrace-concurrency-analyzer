// SPDX-License-Identifier: Apache-2.0
// The loop joins threads[0] only: the thread created into threads[1] is never joined, and main's
// write races with nothing but runs beside it.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void* other(void* argument)
{
    (void)argument;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    pthread_t threads[2];
    pthread_create(&threads[0], NULL, worker, NULL);
    pthread_create(&threads[1], NULL, other, NULL);
    for (int i = 0; i < 1; i++)
        pthread_join(threads[i], NULL);
    shared += 2;
    return shared;
}
