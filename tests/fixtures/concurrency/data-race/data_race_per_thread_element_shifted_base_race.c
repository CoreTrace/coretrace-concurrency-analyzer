// SPDX-License-Identifier: Apache-2.0
// Thread i writes results[i], which is big[i + 1]; before starting thread i, main writes big[i],
// the element thread i - 1 may still be writing (#108).
// Expected: one data race.
#include <pthread.h>
static int big[5];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    int* results = &big[1];
    for (int i = 0; i < 4; ++i)
    {
        big[i] = -1;
        pthread_create(&threads[i], NULL, worker, &results[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return big[0];
}
