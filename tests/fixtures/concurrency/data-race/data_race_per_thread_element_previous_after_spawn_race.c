// SPDX-License-Identifier: Apache-2.0
// After starting thread i, main writes results[i - 1], the element thread i - 1 may still be
// writing (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
    {
        pthread_create(&threads[i], NULL, worker, &results[i]);
        if (i > 0)
            results[i - 1] = -1;
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return results[0];
}
