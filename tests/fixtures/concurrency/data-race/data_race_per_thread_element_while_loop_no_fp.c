// SPDX-License-Identifier: Apache-2.0
// A while loop whose body ends with the increment hands each thread its own element; main reads
// after the join loop (#108).
// Expected: no diagnostic.
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
    int i = 0;
    while (i < 4)
    {
        pthread_create(&threads[i], NULL, worker, &results[i]);
        i++;
    }
    for (int j = 0; j < 4; ++j)
        pthread_join(threads[j], NULL);
    return results[0];
}
