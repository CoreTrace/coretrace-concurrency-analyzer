// SPDX-License-Identifier: Apache-2.0
// The counter advances only every other round, so each element is handed to two threads (#108).
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
    pthread_t threads[8];
    int started = 0;
    for (int i = 0; i < 4;)
    {
        pthread_create(&threads[started], NULL, worker, &results[i]);
        if (++started % 2 == 0)
            ++i;
    }
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return results[0];
}
