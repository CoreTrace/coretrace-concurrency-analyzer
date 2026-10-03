// SPDX-License-Identifier: Apache-2.0
// Each worker clears two ints from its element on, reaching into the element of the next thread
// (#108).
// Expected: one data race.
#include <pthread.h>
static int results[5];
static void* worker(void* argument)
{
    int* slot = argument;
    for (int k = 0; k < 2; ++k)
        slot[k] = 0;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return results[0];
}
