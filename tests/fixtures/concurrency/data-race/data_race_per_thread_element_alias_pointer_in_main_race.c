// SPDX-License-Identifier: Apache-2.0
// main keeps a pointer to results[1] and writes through it after each spawn; from round 1 on,
// thread 1 may be writing results[1] (#108).
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
    int* second = &results[1];
    for (int i = 0; i < 4; ++i)
    {
        pthread_create(&threads[i], NULL, worker, &results[i]);
        *second = -1;
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return results[0];
}
