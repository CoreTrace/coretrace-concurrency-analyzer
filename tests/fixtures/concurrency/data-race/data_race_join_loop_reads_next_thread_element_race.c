// SPDX-License-Identifier: Apache-2.0
// The join loop reads results[i + 1] right after joining thread i: thread i + 1, which writes
// it, may still run (#108).
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
    int total = 0;
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
    {
        pthread_join(threads[i], NULL);
        if (i + 1 < 4)
            total += results[i + 1];
    }
    return total;
}
