// SPDX-License-Identifier: Apache-2.0
// The join loop skips thread 2, then reads results[2], which thread 2 may still be writing; it
// is never joined (#108).
// Expected: one data race and one missing join.
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
    int total = 0;
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
    {
        if (i != 2)
            pthread_join(threads[i], NULL);
        total += results[i];
    }
    return total;
}
