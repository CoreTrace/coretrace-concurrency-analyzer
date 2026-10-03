// SPDX-License-Identifier: Apache-2.0
// The counter is recomputed as started / 2, so each element is handed to two threads (#108).
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
    for (int i = 0; i < 4; i = started / 2)
        pthread_create(&threads[started++], NULL, worker, &results[i]);
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return results[0];
}
