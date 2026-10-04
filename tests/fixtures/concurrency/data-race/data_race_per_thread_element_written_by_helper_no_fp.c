// SPDX-License-Identifier: Apache-2.0
// Each thread hands its own element to a helper that writes it; main reads after the join loop
// (#108).
// Expected: no diagnostic.
#include <pthread.h>
static int results[4];
static void fill(int* target, int value)
{
    *target = value;
}
static void* worker(void* argument)
{
    fill(argument, 7);
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return results[2];
}
