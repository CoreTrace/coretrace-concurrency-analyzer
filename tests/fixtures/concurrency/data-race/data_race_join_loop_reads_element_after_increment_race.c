// SPDX-License-Identifier: Apache-2.0
// The join loop raises its counter between joining thread k and reading results[k]: it reads the
// element of thread k + 1, which may still be writing it (#108).
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
    int k = 0;
    while (k < 4)
    {
        pthread_join(threads[k], NULL);
        ++k;
        if (k < 4)
            total += results[k];
    }
    return total;
}
