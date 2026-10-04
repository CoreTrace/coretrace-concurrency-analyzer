// SPDX-License-Identifier: Apache-2.0
// Before starting thread i, main writes results[previous], previous holding i - 1: the element
// of thread i - 1, which may still be writing it (#108).
// Expected: one data race.
#include <pthread.h>

static int results[4];

static void* worker(void* argument)
{
    int* element = argument;
    *element = 42;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
    {
        int previous = i > 0 ? i - 1 : 0;
        results[previous] = -1;
        pthread_create(&threads[i], NULL, worker, &results[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return 0;
}
