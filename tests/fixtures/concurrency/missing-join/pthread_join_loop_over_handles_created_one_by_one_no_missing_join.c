// SPDX-License-Identifier: Apache-2.0
// Two threads are created one by one into threads[0] and threads[1], and a loop joins both before
// main writes (#161).
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
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);
    shared += 2;
    return shared;
}
