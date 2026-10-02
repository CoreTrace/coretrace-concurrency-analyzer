// SPDX-License-Identifier: Apache-2.0
// worker's thread is created into threads[0], then another thread is created over it: worker's
// thread is never joined, though a loop joins threads[0] and threads[1], and its write races with
// main's.
// Expected: one data race and one missing join.
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
    pthread_create(&threads[0], NULL, other, NULL);
    pthread_create(&threads[1], NULL, other, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);
    shared += 2;
    return shared;
}
