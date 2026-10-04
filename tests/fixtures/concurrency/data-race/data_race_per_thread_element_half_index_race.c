// SPDX-License-Identifier: Apache-2.0
// Threads 2k and 2k+1 are both handed &results[k] (index i / 2) and write it (#108).
// Expected: one data race.
#include <pthread.h>
static int results[2];
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
        pthread_create(&threads[i], NULL, worker, &results[i / 2]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return results[0];
}
