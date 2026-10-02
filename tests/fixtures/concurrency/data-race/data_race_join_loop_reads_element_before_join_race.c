// SPDX-License-Identifier: Apache-2.0
// The join loop reads results[i] before joining thread i, which may still be writing it (#108).
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
        total += results[i];
        pthread_join(threads[i], NULL);
    }
    return total;
}
