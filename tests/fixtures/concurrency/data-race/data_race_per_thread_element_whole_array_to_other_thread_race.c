// SPDX-License-Identifier: Apache-2.0
// An auditor thread, started first, clears every element while the workers write their own (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static void* auditor(void* argument)
{
    int* all = argument;
    for (int k = 0; k < 4; ++k)
        all[k] = 0;
    return NULL;
}
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t audit;
    pthread_t threads[4];
    pthread_create(&audit, NULL, auditor, results);
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    pthread_join(audit, NULL);
    return results[0];
}
